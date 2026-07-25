#ifndef DATABASE_H
#define DATABASE_H

#define UNAVAILABLE_STRING  "[unavailable]"
#define DATA_INDEX_LIMIT    100000

#include <QtSql>

class ItemObject;
typedef QSharedPointer <ItemObject> Item;

class ItemObject
{

public:

    ItemObject(quint32 id, const QString &endpoint, const QString &property, quint32 debounce, double threshold) :
        m_id(id), m_endpoint(endpoint), m_property(property), m_debounce(debounce), m_threshold(threshold), m_timestamp(0) {}

    inline quint32 id(void) { return m_id; }
    inline QString endpoint(void) { return m_endpoint; }
    inline QString property(void) { return m_property; }

    inline qint64 timestamp(void) { return m_timestamp; }
    inline void setTimestamp(qint64 value) { m_timestamp = value; }

    inline quint32 debounce(void) { return m_debounce; }
    inline void setDebounce(quint32 value) { m_debounce = value; }

    inline double threshold(void) { return m_threshold; }
    inline void setThreshold(double value) { m_threshold = value; }

    inline QString value(void) { return m_value; }
    inline void setValue(const QString value) { m_value = value; }

    bool skip(qint64 timestamp, double value);

private:

    quint32 m_id;
    QString m_endpoint, m_property;

    quint32 m_debounce;
    double m_threshold;

    qint64 m_timestamp;
    QString m_value;

};

class Database : public QObject
{
    Q_OBJECT

public:

    Database(QSettings *config, QObject *parent);
    ~Database(void);

    struct DataRecord
    {
        quint32 id;
        qint64  timestamp;
        QString value;
    };

    struct HourRecord
    {
        quint32 id;
        qint64  timestamp;
        QString avg, min, max;
    };

    inline bool debug(void) { return m_debug; }
    inline QMap <QString, Item> &items(void) { return m_items; }

    bool updateItem(const QString &endpoint, const QString &property, quint32 debounce, double threshold);
    bool removeItem(const QString &endpoint, const QString &property);

    void insertData(const Item &item, const QString &value);
    void getData(const Item &item, qint64 start, qint64 end, bool change, QList <DataRecord> &dataList, QList <HourRecord> &hourList);

private:

    QTimer *m_timer;
    QSqlDatabase m_db;
    quint16 m_days;
    bool m_debug;

    QList <QString> m_trigger;
    QMap <QString, Item> m_items;

    QQueue <DataRecord> m_dataQueue;
    QQueue <HourRecord> m_hourQueue;

private slots:

    void update(void);

signals:

    void itemAdded(const Item &item);

};

#endif
