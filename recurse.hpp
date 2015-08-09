#ifndef RECURSE_HPP
#define RECURSE_HPP

#include <QCoreApplication>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QHash>
#include <QStringBuilder>
#include <QVector>
#include <QSharedPointer>
#include <functional>

using std::function;
using std::bind;
using std::ref;

#include "request.hpp"
#include "response.hpp"


typedef function<void(Request &request, Response &response, function<void()> next)> next_f;


//!
//! \brief The Recurse class
//! main class of the app
//!
class Recurse : public QObject
{
public:

    Recurse(int & argc, char ** argv, QObject *parent = NULL);
    ~Recurse();

    bool listen(quint64 port, QHostAddress address = QHostAddress::Any);
    void use(next_f next);
    void end(Request &request, Response &response);

private:
    QCoreApplication app;
    QTcpServer m_tcp_server;

    quint64 m_port;
    QVector<next_f> m_middleware;
    void http_parse(Request &request);
    QString create_reply(Response &response);
    QRegExp httpRx = QRegExp("^(?=[A-Z]).* \\/.* HTTP\\/[0-9]\\.[0-9]\\r\\n");

    struct Client {
        Request request;
        Response response;
    };

    void m_next(QSharedPointer<Client> client, int current_middleware);
};

Recurse::Recurse(int & argc, char ** argv, QObject *parent) : app(argc, argv)
{
    Q_UNUSED(parent);
};

Recurse::~Recurse()
{

};

//!
//! \brief Recurse::listen
//! listen for tcp requests
//!
//! \param port tcp server port
//! \param address tcp server listening address
//!
//! \return true on success
//!
bool Recurse::listen(quint64 port, QHostAddress address)
{
    m_port = port;
    int bound = m_tcp_server.listen(address, port);
    if (!bound)
        return false;

    connect(&m_tcp_server, &QTcpServer::newConnection, [this] {
        qDebug() << "client connected";

        QSharedPointer<Client> client = QSharedPointer<Client>::create();
        client->request.socket = m_tcp_server.nextPendingConnection();

        connect(client->request.socket, &QTcpSocket::readyRead, [this, client] {
            client->request.data += client->request.socket->readAll();
            qDebug() << "client request: " << client->request.data;

            http_parse(client->request);

            if (client->request.body_length < client->request.header["content-length"].toLongLong())
                    return;

            if (m_middleware.count() > 0) {
                client->response.end = bind(&Recurse::end, this, std::ref(client->request), std::ref(client->response));

                m_middleware[0](
                    client->request,
                    client->response,
                    bind(&Recurse::m_next, this, client, 0));
            }
        });
    });

    return app.exec();
};

//!
//! \brief Recurse::m_next
//! call next middleware
//!
void Recurse::m_next(QSharedPointer<Client> client, int current_middleware)
{
    qDebug() << "calling next:" << current_middleware << " num:" << m_middleware.size();

    if (++current_middleware >= m_middleware.size()) {
        return;
    };

    m_middleware[current_middleware](
        client->request,
        client->response,
        bind(&Recurse::m_next, this, client, current_middleware));
};

//!
//! \brief Recurse::use
//! add new middleware
//!
//! \param f middleware function that will be called later
//!
void Recurse::use(next_f f)
{
    m_middleware.push_back(f);
};

//!
//! \brief Recurse::end
//! final function to be called for creating/sending response
//! \param request
//! \param response
//!
void Recurse::end(Request &request, Response &response)
{
    qDebug() << "calling end with:" << response.body();

    QString header;

    response.method = request.method;
    response.proto = request.proto;

    if (response.status() == 0)
        response.status(200);

    QString reply = create_reply(response);

    qDebug() << "create reply" << reply;

    // send response to the client
    qint64 check = request.socket->write(
                reply.toStdString().c_str(),
                reply.size());

    qDebug() << "socket write debug:" << check;
    request.socket->close();
};

//!
//! \brief Recurse::http_parse
//! parse http data
//!
//! \param data reference to data received from the tcp connection
//!
void Recurse::http_parse(Request &request)
{
    // if no header is present, just append all data to request.body
    if (!request.data.contains(httpRx)) {
        request.body.append(request.data);
        return;
    }

    QStringList data_list = request.data.split("\r\n");
    bool is_body = false;

    for (int i = 0; i < data_list.size(); ++i) {
        if (is_body) {
            request.body.append(data_list.at(i));
            request.body_length += request.body.size();
            continue;
        }

        QStringList entity_item = data_list.at(i).split(":");

        if (entity_item.length() < 2 && entity_item.at(0).size() < 1 && !is_body) {
            is_body = true;
            continue;
        }
        else if (i == 0 && entity_item.length() < 2) {
            QStringList first_line = entity_item.at(0).split(" ");
            request.method = first_line.at(0);
            request.url = first_line.at(1).trimmed();
            request.proto = first_line.at(2).trimmed();
            continue;
        }

        request.header[entity_item.at(0).toLower()] = entity_item.at(1).trimmed();
    }

    qDebug() << "request object populated: "
        << request.method << request.url << request.header << request.proto << request.body
        << request.body_length;
};

//!
//! \brief Recurse::create_reply
//! create response data for sending to client
//!
//! \param response reference to the Response instance
//!
QString Recurse::create_reply(Response &response)
{
    qDebug() << "response.header:" << response.header;

    qDebug() << __FUNCTION__ << response.body();

    QString data = response.proto % " " % QString::number(response.status()) % " "
        % response.http_codes[response.status()] % "\r\n";

    // set content length
    response.header["content-length"] = QString::number(response.body().size());

    // set content type if not set
    if (!response.header.contains("content-type"))
        response.header["content-type"] = "text/plain";

    // set custom header fields
    QHash<QString, QString>::const_iterator i;
    for (i = response.header.constBegin(); i != response.header.constEnd(); ++i)
        data += i.key() % ": " % i.value() % "\r\n";

    return data % "\r\n" % response.body() % "\r\n";
}

#endif // RECURSE_HPP
