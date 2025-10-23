#include "database.h"
#include "logger.h"

bool ItemObject::skip(qint64 timestamp, double value)
{
    if (timestamp < m_timestamp + m_debounce * 1000)
    {
        double check = m_value.toDouble();
        return !m_threshold || (check < value && check + m_threshold > value) || (check > value && check - m_threshold < value);
    }

    return false;
}

Database::Database(QSettings *config, QObject *parent) : QObject(parent), m_timer(new QTimer(this)), m_db(QSqlDatabase::addDatabase("QSQLITE", "db"))
{
    QSqlQuery query(m_db);

    m_db.setDatabaseName(config->value("database/file", "/opt/homed-recorder/homed-recorder.db").toString());
    m_days = static_cast <quint16> (config->value("database/days").toInt());
    m_debug = config->value("database/debug", false).toBool();
    m_trigger = {"action", "event", "scene"};

    if (!m_days)
        m_days = 7;

    logInfo << "Using database" << m_db.databaseName() << "with" << m_days << "days purge inerval";

    if (!m_db.open())
    {
        logWarning << "Database open error";
        return;
    }

    query.exec("CREATE TABLE IF NOT EXISTS item (id INTEGER PRIMARY KEY AUTOINCREMENT, endpoint TEXT NOT NULL, property TEXT NOT NULL, debounce INTEGER NOT NULL, threshold REAL NOT NULL)");
    query.exec("CREATE TABLE IF NOT EXISTS data (id INTEGER PRIMARY KEY AUTOINCREMENT, item_id INTEGER REFERENCES item(id) ON DELETE CASCADE, timestamp INTEGER NOT NULL, value TEXT NOT NULL)");
    query.exec("CREATE TABLE IF NOT EXISTS hour (id INTEGER PRIMARY KEY AUTOINCREMENT, item_id INTEGER REFERENCES item(id) ON DELETE CASCADE, timestamp INTEGER NOT NULL, avg REAL NOT NULL, min REAL NOT NULL, max REAL NOT NULL)");
    query.exec("CREATE UNIQUE INDEX item_index ON item (endpoint, property)");

    query.exec("PRAGMA foreign_keys = ON");
    query.exec("SELECT * FROM item");

    while (query.next())
    {
        Item item(new ItemObject(static_cast <quint32> (query.value(0).toInt()), query.value(1).toString(), query.value(2).toString(), static_cast <quint32> (query.value(3).toInt()), query.value(4).toDouble()));
        m_items.insert(QString("%1/%2").arg(item->endpoint(), item->property()), item);
    }

    connect(m_timer, &QTimer::timeout, this, &Database::update);
    m_timer->start(1000);
}

Database::~Database(void)
{
    m_db.close();
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase("db");
}

bool Database::updateItem(const QString &endpoint, const QString &property, quint32 debounce, double threshold)
{
    QString key = QString("%1/%2").arg(endpoint, property);
    QSqlQuery query(m_db);

    if (m_items.contains(key))
    {
        const Item &item = m_items.value(key);

        if (!query.exec(QString("UPDATE item SET debounce = %1, threshold = %2 WHERE id = %3").arg(debounce).arg(threshold).arg(item->id())))
            return false;

        item->setDebounce(debounce);
        item->setThreshold(threshold);
    }
    else
    {
        if (!query.exec(QString("INSERT INTO item (endpoint, property, debounce, threshold) VALUES ('%1', '%2', %3, %4)").arg(endpoint, property).arg(debounce).arg(threshold)))
            return false;

        m_items.insert(key, Item(new ItemObject(static_cast <qint32> (query.lastInsertId().toInt()), endpoint, property, debounce, threshold)));
        emit itemAdded(m_items.value(key));
    }

    return true;
}

bool Database::removeItem(const QString &endpoint, const QString &property)
{
    auto it = m_items.find(QString("%1/%2").arg(endpoint, property));
    QSqlQuery query(m_db);

    if (it == m_items.end() || !query.exec(QString("DELETE FROM item WHERE id = %1").arg(it.value()->id())))
        return false;

    m_items.erase(it);
    return true;
}

void Database::insertData(const Item &item, const QString &value)
{
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();

    if (!item->timestamp())
    {
        QSqlQuery query(QString("SELECT timestamp, value FROM data WHERE item_id = %1 ORDER BY id DESC LIMIT 1").arg(item->id()), m_db);

        if (query.first())
        {
            item->setTimestamp(query.value(0).toLongLong());
            item->setValue(query.value(1).toString());
        }
    }

    if (item->timestamp() > timestamp || (item->value() == value && !m_trigger.contains(item->property())) || item->skip(timestamp, value.toDouble()))
    {
        if (m_debug)
            logInfo << "Endpoint" << item->endpoint() << "property" << item->property() << "value" << value << "ignored";

        return;
    }

    m_dataQueue.enqueue({item->id(), timestamp, value});

    if (m_debug)
        logInfo << "Endpoint" << item->endpoint() << "property" << item->property() << "value" << value << "record enqueued";

    item->setTimestamp(timestamp);
    item->setValue(value);
}

void Database::getData(const Item &item, qint64 start, qint64 end, QList <DataRecord> &dataList, QList <HourRecord> &hourList)
{
    QSqlQuery query(m_db);
    QString queryString;
    bool check = false;
    qint64 last = 0;

    if (start && m_days >= (QDateTime::currentMSecsSinceEpoch() - start) / 86400000)
    {
        queryString = QString("SELECT timestamp, value FROM data WHERE item_id = %1").arg(item->id());
        query.exec(QString(queryString).append(" AND timestamp <= %1 ORDER BY id DESC LIMIT 1").arg(start));

        if (query.first())
            dataList.append({item->id(), query.value(0).toLongLong(), query.value(1).toString()});

        check = true;
    }
    else
        queryString = QString("SELECT timestamp, avg, min, max FROM hour WHERE item_id = %1").arg(item->id());

    if (start)
        queryString.append(QString(" AND timestamp > %1").arg(start));

    if (end)
        queryString.append(QString(" AND timestamp <= %1").arg(end));

    query.exec(queryString);

    while (query.next())
    {
        qint64 timestamp = query.value(0).toLongLong();

        if (last > timestamp)
            continue;

        if (check)
            dataList.append({item->id(), query.value(0).toLongLong(), query.value(1).toString()});
        else
            hourList.append({item->id(), query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toString(), query.value(3).toString()});

        last = timestamp;
    }
}

void Database::update(void)
{
    qint64 timestamp = QDateTime::currentSecsSinceEpoch(), start = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(m_db);

    query.exec("BEGIN TRANSACTION");

    while (!m_dataQueue.isEmpty())
    {
        DataRecord record = m_dataQueue.dequeue();
        query.exec(QString("INSERT INTO data (item_id, timestamp, value) VALUES (%1, %2, '%3')").arg(record.id).arg(record.timestamp).arg(record.value));
    }

    query.exec("COMMIT");

    if (timestamp % 3600)
        return;

    query.exec("SELECT count(*) from data");

    if (query.first() && query.value(0).toInt() > DATA_INDEX_LIMIT)
    {
        query.exec("CREATE INDEX data_index ON data (item_id, timestamp)");
        query.exec("REINDEX data");
    }

    query.exec(QString("SELECT item.id, AVG(data.value), MIN(data.value), MAX(data.value) FROM item LEFT JOIN data ON data.item_id = item.id AND data.timestamp > %1 GROUP by item.id").arg((timestamp - 3600) * 1000));

    while (query.next())
    {
        quint32 id = static_cast <quint32> (query.value(0).toInt());

        if (query.value(1).type() == QVariant::Double)
        {
            bool min, max;

            query.value(2).toString().toDouble(&min);
            query.value(3).toString().toDouble(&max);

            if (!min || !max)
                continue;

            m_hourQueue.enqueue({id, timestamp * 1000, query.value(1).toString(), query.value(2).toString(),query.value(3).toString()});
        }
        else
        {
            QSqlQuery query(QString("SELECT value FROM data WHERE item_id = %1 ORDER BY id DESC limit 1").arg(id), m_db);

            if (!query.first() || query.value(0).toString() == UNAVAILABLE_STRING)
                continue;

            query.exec(QString("SELECT avg, min, max FROM hour WHERE item_id = %1 ORDER BY id DESC limit 1").arg(id));

            if (!query.first())
                continue;

            m_hourQueue.enqueue({id, timestamp * 1000, query.value(0).toString(), query.value(1).toString(), query.value(2).toString()});
        }
    }

    query.exec("BEGIN TRANSACTION");

    while (!m_hourQueue.isEmpty())
    {
        const HourRecord &record = m_hourQueue.dequeue();
        query.exec(QString("INSERT INTO hour (item_id, timestamp, avg, min, max) VALUES (%1, %2, %3, %4, %5)").arg(record.id).arg(record.timestamp).arg(record.avg, record.min, record.max));
    }

    query.exec("COMMIT");
    logInfo << "Hour data stored in" << QDateTime::currentMSecsSinceEpoch() - start << "ms";

    if (QDateTime::currentDateTime().time().hour())
        return;

    query.exec(QString("DELETE FROM data WHERE timestamp < %1 AND ID NOT IN (SELECT MAX(id) FROM data WHERE timestamp < %1 GROUP BY item_id)").arg((timestamp - m_days * 86400) * 1000));
    query.exec("VACUUM");
}
