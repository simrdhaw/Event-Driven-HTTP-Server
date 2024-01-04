
#include <iostream>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <vector>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <unordered_map>
#include <sys/stat.h>
#include <regex>
#include <optional>

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::stringstream;

int BUFFER_SIZE = 1025;

/* Function returns true if the src filetype is same as test file type.
   This is used to checking which file type is requested by client.
*/
bool EndsWith(const string &src, const string &test)
{
    if (src.size() < test.size())
    {
        // cout << "len: " << src.size() << " < " << test.size() << endl;
        return false;
    }
    for (int i = test.size() - 1, j = src.size() - 1; i >= 0 && j >= 0; i--, j--)
    {
        if (test[i] != src[j])
        {
            // cout << test[i] << " != " << src[j] << endl;
            return false;
        }
    }
    return true;
}

/* This is used to parse the request sent by client. The client request should be in format: GET /index.shtml HTTP/1.x, x =0,1
    This will return filename requested by client or nullopt if request is a bad request*/
std::optional<std::string> ParseInputRequest(const string &request)
{
    stringstream ss(request);
    string token;
    int cnt = 0;
    string filename;
    bool is_valid = true;
    while (ss >> token)
    {
        cout << "request token: " << token << endl;
        if (cnt >= 3)
        {
            // cout << "Not parsing further request params" << endl;
            break;
        }
        else if (cnt == 0)
        {
            if (token != "GET")
            {
                cout << "invaid input by client" << endl;
                is_valid = false;
                break;
            }
        }
        else if (cnt == 1)
        {
            filename = token;
        }
        else
        {
            if (token != "HTTP/1.0" && token != "HTTP/1.1")
            // if (token != "HTTP/1.0")
            {
                cout << "invaid input by client: " << token << endl;
                is_valid = false;
            }
        }
        cnt++;
    }
    if (is_valid)
    {
        return filename;
    }
    return std::nullopt;
}

/* The response structure which contains all variables(headers and content) and functions to create the response corresponding to different
    requests by clients */
struct Response
{
    /* This function sets the status, depending on error or message ok and set the appropriate content type*/
    void SetStatus(int status)
    {
        if (status == 200)
        {
            this->status = "HTTP/1.1 " + std::to_string(status) + " OK";
        }
        else if (status == 400)
        {
            this->status = "HTTP/1.1 " + std::to_string(status) + " Bad Request";
            content_type = "text/plain";
            contents = "Bad Request";
        }
        else if (status == 403)
        {
            this->status = "HTTP/1.1 " + std::to_string(status) + " Forbidden";
            content_type = "text/plain";
            contents = "Permission error";
        }
        else if (status == 404)
        {
            this->status = "HTTP/1.1 " + std::to_string(status) + " Not Found";
            content_type = "text/plain";
            contents = "Page not found";
        }
    }

    /* This function adds the actual content of files in the response generated*/
    void AddContent(const string &value)
    {
        contents = value;
    }

    /* This function checks for file type to set apporopriate content type in Response header*/
    bool IfIsValidSetContentType(const string &path)
    {
        if (EndsWith(path, ".html"))
        {
            content_type = "text/html";
        }
        else if (EndsWith(path, ".txt"))
        {
            content_type = "text/plain";
        }
        else if (EndsWith(path, ".xml"))
        {
            content_type = "text/xml";
        }
        else if (EndsWith(path, ".csv"))
        {
            content_type = "text/csv";
        }
        else if (EndsWith(path, ".css"))
        {
            content_type = "text/css";
        }
        else if (EndsWith(path, ".jpg") || EndsWith(path, ".jpeg"))
        {
            content_type = "image/jpeg";
        }
        else if (EndsWith(path, ".png"))
        {
            content_type = "image/png";
        }
        else if (EndsWith(path, ".gif"))
        {
            content_type = "image/gif";
        }
        else if (EndsWith(path, ".js"))
        {
            content_type = "text/javascript";
        }
        else
        {
            return false;
        }
        return true;
    }

    /*This finds the timeout time for HTTP/1.1 message heuristically depending of number of active connections and \
    maximum number of connections. Atleast a tiemout is of 5sec and maximum value is 5 + max active connections*/
    void SetKeepAliveTimeout(int timeout)
    {
        keep_alive = "Keep-Alive: timeout=" + std::to_string(timeout) + ", max=0";
    }

    /* Calculates the current date and time for HTTP-time header*/
    void SetDateTime()
    {
        time_t present = time(0);
        char time_buff[1025] = {0};
        struct tm tm = *gmtime(&present);
        strftime(time_buff, sizeof(time_buff), "%a, %d %b %Y %H:%M:%S %Z", &tm);
        http_date = string(time_buff);
    }

    /*This function concatenates all the response headers and the conetent of file to be sent to client */
    string ToString()
    {
        std::stringstream ss;
        ss << status << "\r\n";
        ss << "Content-Type: " << content_type << "\r\n";
        ss << "Content-Length: " << contents.size() << "\r\n";
        ss << "Connection: Keep-Alive"
           << "\r\n";
        ss << keep_alive << "\r\n";
        ss << "Date: " << http_date << "\r\n";
        ss << "\r\n"
           << contents;
        cout << "returning:\n"
             << status << "\n"
             << "Content-Type: " << content_type << "\n"
             << "Content-Length: " << contents.size() << "\n"
             << "Connection: Keep-Alive"
             << "\n"
             << keep_alive << "\n"
             << "Date: " << http_date << "\n";
        return ss.str();
    }

    string status;
    string contents;
    string content_type;
    string keep_alive;
    string http_date;
};

struct WebServerOptions
{
    int port;
    int max_active_connections;
    int max_pending_connections;
    string document_root;
};

/*This function checks for following things for a file requested by client:
    1. If file exists at a particular location
    2. If the existing file is world readable */
int CheckFileExistsAndPermissions(const std::string &path)
{
    std::filesystem::path file_path(path);
    bool file_exists = std::filesystem::exists(file_path);

    // File does not exist.
    if (!file_exists)
    {
        return 404;
    }

    std::filesystem::file_status status = std::filesystem::status(file_path);
    if ((status.permissions() & std::filesystem::perms::others_read) == std::filesystem::perms::none)
    {
        return 403;
    }
    return 200;
}

/*Clears the buffer on which it receives request from a client*/
void ClearBuffer(char *buffer)
{
    for (int i = 0; i < BUFFER_SIZE; i++)
    {
        buffer[i] = '\0';
    }
}

void ConvertLinksToFilePath(std::vector<string> &hyperlinked_files, std::vector<string> &hyperlinked_files_path)
{
    cout << "SIZE" << hyperlinked_files.size() << endl;
    int cnt = 0;
    for (int i = 0; i < hyperlinked_files.size(); i++)
    {
        if (hyperlinked_files[i].size() < 20)
        {
            continue;
        }
        string scu_links = hyperlinked_files[i].substr(0, 20);
        if (scu_links == "https://www.scu.edu/")
        {
            string scu_links_val = hyperlinked_files[i].substr(20);
            //  cout<<"i"<<i<<" "<<scu_links_val<<endl;
            string scu_link;
            std::stringstream mystream(scu_links_val);
            if (getline(mystream, scu_link, '"'))
            {
                cout << "i" << i << " " << scu_link << endl;
                hyperlinked_files_path.push_back(scu_link);
            }
            else
            {
                cout << "i" << i << " " << scu_links_val << endl;
                hyperlinked_files_path.push_back(scu_links_val);
            }
            cnt++;
        }
    }
}

/* Creates the output response with all the headers and content */
void CreateOutput(string pageName, const string &document_root, Response &resp)
{
    if (EndsWith(pageName, "/"))
    {
        pageName += "index.html";
    }
    string absolute_file_path = document_root + "/" + pageName;
    int result = CheckFileExistsAndPermissions(absolute_file_path);
    if (result != 200)
    {
        cout << "File " << absolute_file_path << " : " << result << endl;
        resp.SetStatus(result);
        return;
    }

    std::ifstream orignal_page(absolute_file_path);
    std::ostringstream buff;
    buff << orignal_page.rdbuf();
    cout << "Checking pageName: " << pageName << endl;
    if (resp.IfIsValidSetContentType(pageName))
    {
        resp.AddContent(buff.str());
        resp.SetStatus(200);
    }
    else
    {
        resp.SetStatus(400);
        // return;
    }
    cout << "status: " << resp.status << endl;
}

/* The function parses the arguments for the server program and extracts the port number to listen and gets the document root*/
WebServerOptions ParseArguments(int argc, char *argv[])
{
    if (argc != 5)
    {
        cout << "The program should have 4 input arguments\n";
        exit(1);
    }
    std::stringstream mystream_1(argv[1]);
    std::stringstream mystream_3(argv[3]);
    std::stringstream mystream_port;
    std::stringstream mystream_document_root;

    if (mystream_1.str() != "-document_root")
    {
        if (mystream_1.str() != "-port")
        {
            std::cout << "Incorrect server command try sending request in format : ./server -document_root path of file -port 8888"
                      << endl;
            exit(1);
        }
        else
        {
            mystream_port << argv[2];
            if (mystream_3.str() != "-document_root")
            {
                std::cout << "Incorrect server command try sending request in format : ./server -document_root path of file -port 8888"
                          << endl;
                exit(1);
            }
            else
            {
                mystream_document_root << argv[4];
            }
        }
    }
    else if (mystream_1.str() == "-document_root")
    {
        if (mystream_3.str() != "-port")
        {
            std::cout << "Incorrect server command try sending request in format : ./server -document_root path of file -port 8888"
                      << endl;
            exit(1);
        }
        else
        {
            mystream_port << argv[4];
            mystream_document_root << argv[2];
        }
    }
    else
    {
        std::cout << "Incorrect server command try sending request in format : ./server -document_root path of file -port 8888"
                  << endl;
        exit(1);
    }

    int port_value = stoi(mystream_port.str());
    if (port_value > 9999 || port_value < 8000)
    {
        std::cout << "Incorrect server port number try sending request at port 8000 to 9999"
                  << endl;
        exit(1);
    }
    WebServerOptions server_options{.port = stoi(mystream_port.str()), .max_active_connections = 8, .max_pending_connections = 4, .document_root = mystream_document_root.str() /*"/home/sdhawan/data/" "/mnt/d/simrdhaw/Distributed Systems/Assignments/Programming Assignment 1/code/data/"*/};
    return server_options;
}

// Keep connection open for at least 5 seconds.
// If max active connections is 10, and currently 7 clients are connected, keep the connection open for (5 + 10 - 7 = 8 sec).
int GetTimeoutForConnection(int num_curr_connections, int max_active_connections)
{
    int timeout = 5 + (max_active_connections - num_curr_connections);
    return timeout;
}

int main(int argc, char *argv[])
{
    WebServerOptions server_options = ParseArguments(argc, argv);
    std::vector<int> client_socket(server_options.max_active_connections);

    int opt = 1;
    int master_socket;
    struct sockaddr_in address;

    char buffer[BUFFER_SIZE] = {0};

    // set of socket descriptors
    fd_set socket_fds;

    if ((master_socket = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        cout << "Failed to create socket\n";
        exit(1);
    }

    if (setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, (char *)&opt,
                   sizeof(opt)) < 0)
    {
        cout << "Failed to set socket options\n";
        exit(1);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(server_options.port);

    if (bind(master_socket, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        cout << "Failed to bind socket to port " << server_options.port << endl;
        exit(1);
    }

    if (listen(master_socket, server_options.max_pending_connections) < 0)
    {
        cout << "Failed to set max pending connections on socket" << endl;
        exit(1);
    }

    int addrlen = sizeof(address);
    cout << "Started server to listen for requests on port " << server_options.port << endl;
    int num_active_connections = 0;
    int new_socket;
    int activity;
    int valread;
    int sd;
    int max_sd;

    while (true)
    {
        // clear the socket set for initailizing
        FD_ZERO(&socket_fds);

        // add master socket to set it to listen and monitor the connections
        FD_SET(master_socket, &socket_fds);
        max_sd = master_socket;

        // add child sockets to handle differnt clients
        for (int i = 0; i < server_options.max_active_connections; i++)
        {
            // socket descriptor
            sd = client_socket[i];

            // if valid socket descriptor then add to read list
            if (sd > 0)
                FD_SET(sd, &socket_fds);

            // highest file descriptor number, need it for the select function
            if (sd > max_sd)
                max_sd = sd;
        }

        // Wait for an activity on one of the sockets.
        activity = select(max_sd + 1, &socket_fds, NULL, NULL, NULL);

        if ((activity < 0) && (errno != EINTR))
        {
            cout << "Internal error in select"<<endl;
        }

        // Master socket checking new connection
        if (FD_ISSET(master_socket, &socket_fds))
        {
            if ((new_socket = accept(master_socket,
                                     (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0)
            {
                cout << "Failed to accept new connection" << endl;
                exit(1);
            }

            cout << "Establish new connection , socket fd:" << new_socket
                 << ", ip:" << inet_ntoa(address.sin_addr)
                 << ", port:" << ntohs(address.sin_port)
                 << endl;

            // Adding client socket
            for (int i = 0; i < server_options.max_active_connections; i++)
            {
                if (client_socket[i] == 0)
                {
                    client_socket[i] = new_socket;
                    num_active_connections++;
                    cout << "Add new socket connection for client and num_active_connections: "<< num_active_connections << endl;
                    break;
                }
            }
        }

        // Received request on one of the active client socket
        for (int i = 0; i < server_options.max_active_connections; i++)
        {
            sd = client_socket[i];
            
            if (FD_ISSET(sd, &socket_fds))
            {
                ClearBuffer(buffer);
                //Handling closing request
                if ((valread = read(sd, buffer, 1024)) == 0)
                {
                    getpeername(sd, (struct sockaddr *)&address, (socklen_t *)&addrlen);
                    cout << "disconnected ip: " << inet_ntoa(address.sin_addr) << ", port: " << ntohs(address.sin_port) << endl;
                    close(sd);
                    client_socket[i] = 0;
                    num_active_connections--;
                    cout << "num_active_connections: " << num_active_connections << endl;
                }
                //Handling messages
                else
                {
                    cout << "Received at server: " << std::string(buffer) << endl;
                    Response resp;
                    resp.SetKeepAliveTimeout(GetTimeoutForConnection(num_active_connections, server_options.max_active_connections));
                    std::optional<string> request_parsed = ParseInputRequest(std::string(buffer));
                    bool is_valid = request_parsed.has_value();
                    std::string pageName;
                    resp.SetDateTime();
                    if (is_valid)
                    {
                        pageName = request_parsed.value();
                    }
                    else
                    {
                        cout << "is_valid is false" << endl;
                        resp.SetStatus(400);
                        string response = resp.ToString();
                        send(sd, response.c_str(), response.size(), 0);
                        continue;
                    }

                    CreateOutput(pageName, server_options.document_root, resp);
                    string response = resp.ToString();
                    send(sd, response.c_str(), response.size(), 0);
                }
            }
        }
    }

    return 0;
}
