#include "controller.h"
#include "logger.h"

Controller::Controller(const QString &configFile) : HOMEd(configFile), m_database(new Database(getConfig(), this)), m_commands(QMetaEnum::fromType <Command> ())
{
    logInfo << "Starting version" << SERVICE_VERSION;
    logInfo << "Configuration file is" << getConfig()->fileName();

    m_types = {"zigbee", "modbus", "custom"};
    connect(m_database, &Database::itemAdded, this, &Controller::itemAdded);
}

Device Controller::findDevice(const QString &search)
{
    for (auto it = m_devices.begin(); it != m_devices.end(); it++)
        if (search == it.value()->key() || search.startsWith(it.value()->key().append('/')) || search == it.value()->topic() || search.startsWith(it.value()->topic().append('/')))
            return it.value();

    return Device();
}

void Controller::publishItems(void)
{
    QJsonArray items;

    for (auto it = m_database->items().begin(); it != m_database->items().end(); it++)
        items.append(QJsonObject {{"endpoint", it.value()->endpoint()}, {"property", it.value()->property()}, {"debounce", QJsonValue::fromVariant(it.value()->debounce())}, {"threshold", it.value()->threshold()}});

    mqttPublish(mqttTopic("status/recorder"), {{"items", items}, {"timestamp", QDateTime::currentSecsSinceEpoch()}, {"version", SERVICE_VERSION}}, true);
}

void Controller::mqttConnected(void)
{
    mqttSubscribe(mqttTopic("command/recorder"));
    mqttSubscribe(mqttTopic("status/#"));

    m_devices.clear();
    publishItems();

    mqttPublishStatus();
}

void Controller::mqttReceived(const QByteArray &message, const QMqttTopicName &topic)
{
    QString subTopic = topic.name().replace(mqttTopic(), QString());
    QJsonObject json = QJsonDocument::fromJson(message).object();

    if (subTopic == "command/recorder") // TODO: publish events
    {
        switch (static_cast <Command> (m_commands.keyToValue(json.value("action").toString().toUtf8().constData())))
        {
            case Command::restartService:
            {
                logWarning << "Restart request received...";
                mqttPublish(topic.name(), QJsonObject(), true);
                QCoreApplication::exit(EXIT_RESTART);
                break;
            }
            case Command::updateItem:
            {
                if (!m_database->updateItem(json.value("endpoint").toString(), json.value("property").toString(), static_cast <quint32> (json.value("debounce").toInt()), json.value("threshold").toDouble()))
                {
                    logWarning << "update item request failed";
                    break;
                }

                publishItems();
                break;
            }
            case Command::removeItem:
            {
                if (!m_database->removeItem(json.value("endpoint").toString(), json.value("property").toString()))
                {

                    logWarning << "remove item request failed";
                    break;
                }

                publishItems();
                break;
            }
            case Command::getData:
            {
                const Item &item = m_database->items().value(QString("%1/%2").arg(json.value("endpoint").toString(), json.value("property").toString()));
                QList <Database::DataRecord> dataList;
                QList <Database::HourRecord> hourList;
                qint64 time = QDateTime::currentMSecsSinceEpoch();

                if (!item.isNull())
                    m_database->getData(item, json.value("start").toVariant().toLongLong(), json.value("end").toVariant().toLongLong(), dataList, hourList);

                if (!hourList.count())
                {
                    QJsonArray timestamp, value;

                    for (int i = 0; i < dataList.count(); i++)
                    {
                        const Database::DataRecord &record = dataList.at(i);
                        timestamp.append(record.timestamp);
                        value.append(record.value == UNAVAILABLE_STRING ? QJsonValue::Null : QJsonValue::fromVariant(record.value));
                    }

                    mqttPublish(mqttTopic("recorder"), {{"id", json.value("id").toString()}, {"time", QDateTime::currentMSecsSinceEpoch() - time}, {"timestamp", timestamp}, {"value", value}});
                }
                else
                {
                    QJsonArray timestamp, avg, min, max;

                    for (int i = 0; i < hourList.count(); i++)
                    {
                        const Database::HourRecord &record = hourList.at(i);
                        timestamp.append(record.timestamp);
                        avg.append(record.avg.isEmpty() ? QJsonValue::Null : QJsonValue::fromVariant(record.avg.toDouble()));
                        min.append(record.min.isEmpty() ? QJsonValue::Null : QJsonValue::fromVariant(record.min.toDouble()));
                        max.append(record.max.isEmpty() ? QJsonValue::Null : QJsonValue::fromVariant(record.max.toDouble()));
                    }

                    mqttPublish(mqttTopic("recorder"), {{"id", json.value("id").toString()}, {"time", QDateTime::currentMSecsSinceEpoch() - time}, {"timestamp", timestamp}, {"avg", avg}, {"min", min}, {"max", max}});
                }

                break;
            }
        }
    }
    else if (subTopic.startsWith("status/"))
    {
        QString type = subTopic.split('/').value(1), service = subTopic.mid(subTopic.indexOf('/') + 1);
        QJsonArray devices = json.value("devices").toArray();
        bool names = json.value("names").toBool();

        if (!m_types.contains(type))
            return;

        for (auto it = devices.begin(); it != devices.end(); it++)
        {
            QJsonObject device = it->toObject();
            QString name = device.value("name").toString(), id, key, topic;
            bool check = false;

            if (type == "zigbee" && (device.value("removed").toBool() || !device.value("logicalType").toInt()))
                continue;

            switch (m_types.indexOf(type))
            {
                case 0: id = device.value("ieeeAddress").toString(); break; // zigbee
                case 1: id = QString("%1.%2").arg(device.value("portId").toInt()).arg(device.value("slaveId").toInt()); break; // modbus
                case 2: id = device.value("id").toString(); break; // custom
            }

            if (name.isEmpty())
                name = id;

            key = QString("%1/%2").arg(type, id);
            topic = QString("%1/%2").arg(service, names ? name : id);

            if (m_devices.contains(key) && m_devices.value(key)->topic() != topic)
            {
                const Device &device = m_devices.value(key);
                mqttUnsubscribe(mqttTopic("device/%1").arg(device->topic()));
                mqttUnsubscribe(mqttTopic("fd/%1").arg(device->topic()));
                mqttUnsubscribe(mqttTopic("fd/%1/#").arg(device->topic()));
                device->setTopic(topic);
                check = true;
            }

            if (!m_devices.contains(key))
            {
                m_devices.insert(key, Device(new DeviceObject(key, topic)));
                check = true;
            }

            if (check)
            {
                mqttSubscribe(mqttTopic("device/%1").arg(topic));
                mqttSubscribe(mqttTopic("fd/%1").arg(topic));
                mqttSubscribe(mqttTopic("fd/%1/#").arg(topic));
            }
        }
    }
    else if (subTopic.startsWith("device/"))
    {
        const Device &device = findDevice(subTopic.mid(subTopic.indexOf('/') + 1));

        if (!device.isNull())
        {
            bool status = json.value("status").toString() == "online" ? true : false;

            if (device->available() == status)
                return;

            device->setAvailable(status);

            if (device->available())
            {
                mqttPublish(mqttTopic("command/%1").arg(device->topic().mid(0, device->topic().lastIndexOf('/'))), {{"action", "getProperties"}, {"device", device->topic().split('/').last()}, {"service", "recorder"}});
                return;
            }

            for (auto it = m_database->items().begin(); it != m_database->items().end(); it++)
            {
                if (!it.key().startsWith(device->key()))
                    continue;

                m_database->insertData(it.value(), UNAVAILABLE_STRING);
            }
        }
    }
    else if (subTopic.startsWith("fd/"))
    {
        const Device &device = findDevice(subTopic.mid(subTopic.indexOf('/') + 1));

        if (!device.isNull())
        {
            quint8 endpointId = static_cast <quint8> (subTopic.split('/').last().toInt());
            QString key = endpointId ? QString("%1/%2").arg(device->key()).arg(endpointId) : device->key();

            for (auto it = json.begin(); it != json.end(); it++)
            {
                const Item &item = m_database->items().value(QString("%1/%2").arg(key, it.key()));

                if (item.isNull())
                    continue;

                if (m_database->debug())
                    logInfo << "Endpoint" << endpointId << "property" << it.key() << "item found";

                m_database->insertData(item, it.value().toVariant().toString());
            }
        }
    }
}

void Controller::itemAdded(const Item &item)
{
    const Device &device = findDevice(item->endpoint());

    if (device.isNull() || !device->available())
    {
        m_database->insertData(item, UNAVAILABLE_STRING);
        return;
    }

    mqttPublish(mqttTopic("command/%1").arg(device->topic().mid(0, device->topic().lastIndexOf('/'))), {{"action", "getProperties"}, {"device", device->topic().split('/').last()}, {"service", "recorder"}});
}
