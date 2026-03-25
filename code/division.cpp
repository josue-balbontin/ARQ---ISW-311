#include <winsock2.h>
#include <ws2tcpip.h>
#include <sqlext.h>

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "odbc32.lib")

namespace {

constexpr const char* CONNECTION_STRING =
	"DRIVER={ODBC Driver 17 for SQL Server};"
	"SERVER=localhost;"
	"DATABASE=division;"
	"Trusted_Connection=Yes;";

bool parseDouble(const std::string& value, double& out) {
	char* endPtr = nullptr;
	out = std::strtod(value.c_str(), &endPtr);
	return endPtr != value.c_str() && *endPtr == '\0';
}

std::string getQueryParam(const std::string& query, const std::string& key) {
	std::string pattern = key + "=";
	size_t start = 0;

	while (start < query.size()) {
		size_t end = query.find('&', start);
		if (end == std::string::npos) {
			end = query.size();
		}

		std::string part = query.substr(start, end - start);
		if (part.rfind(pattern, 0) == 0) {
			return part.substr(pattern.size());
		}

		start = end + 1;
	}

	return "";
}

std::string buildHttpResponse(int statusCode, const std::string& statusText, const std::string& body) {
	std::ostringstream response;
	response << "HTTP/1.1 " << statusCode << " " << statusText << "\r\n";
	response << "Access-Control-Allow-Origin: *\r\n";
	response << "Access-Control-Allow-Methods: GET, OPTIONS\r\n";
	response << "Access-Control-Allow-Headers: Content-Type\r\n";
	response << "Content-Type: application/json\r\n";
	response << "Content-Length: " << body.size() << "\r\n";
	response << "Connection: close\r\n\r\n";
	response << body;
	return response.str();
}

bool initDatabaseConnection(SQLHENV& env, SQLHDBC& dbc) {
	if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env))) {
		return false;
	}

	if (!SQL_SUCCEEDED(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<void*>(SQL_OV_ODBC3), 0))) {
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		return false;
	}

	if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc))) {
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		return false;
	}

	SQLCHAR outConnStr[1024];
	SQLSMALLINT outConnStrLen = 0;
	SQLRETURN ret = SQLDriverConnectA(
		dbc,
		nullptr,
		reinterpret_cast<SQLCHAR*>(const_cast<char*>(CONNECTION_STRING)),
		SQL_NTS,
		outConnStr,
		sizeof(outConnStr),
		&outConnStrLen,
		SQL_DRIVER_NOPROMPT
	);

	if (!SQL_SUCCEEDED(ret)) {
		SQLFreeHandle(SQL_HANDLE_DBC, dbc);
		SQLFreeHandle(SQL_HANDLE_ENV, env);
		return false;
	}

	return true;
}

bool savePeticion(SQLHDBC dbc, double a, double b, double resultado) {
	SQLHSTMT stmt = SQL_NULL_HSTMT;
	if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) {
		return false;
	}

	SQLCHAR query[] = "INSERT INTO peticiones (a, b, resultado) VALUES (?, ?, ?)";
	if (!SQL_SUCCEEDED(SQLPrepareA(stmt, query, SQL_NTS))) {
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return false;
	}

	if (!SQL_SUCCEEDED(SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &a, 0, nullptr))) {
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return false;
	}

	if (!SQL_SUCCEEDED(SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &b, 0, nullptr))) {
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return false;
	}

	if (!SQL_SUCCEEDED(SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &resultado, 0, nullptr))) {
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return false;
	}

	if (!SQL_SUCCEEDED(SQLExecute(stmt))) {
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return false;
	}

	SQLFreeHandle(SQL_HANDLE_STMT, stmt);
	return true;
}

std::string handleRequest(const std::string& request, SQLHDBC connection) {
	std::istringstream stream(request);
	std::string method;
	std::string pathAndQuery;
	std::string version;

	stream >> method >> pathAndQuery >> version;

	if (method == "OPTIONS") {
		return buildHttpResponse(204, "No Content", "");
	}

	if (method != "GET") {
		return buildHttpResponse(405, "Method Not Allowed", "{\"error\":\"Solo se permite GET\"}");
	}

	size_t queryPos = pathAndQuery.find('?');
	std::string path = pathAndQuery.substr(0, queryPos);
	std::string query = (queryPos == std::string::npos) ? "" : pathAndQuery.substr(queryPos + 1);

	if (path != "/division") {
		return buildHttpResponse(404, "Not Found", "{\"error\":\"Ruta no encontrada\"}");
	}

	std::string aValue = getQueryParam(query, "a");
	std::string bValue = getQueryParam(query, "b");

	if (aValue.empty() || bValue.empty()) {
		return buildHttpResponse(400, "Bad Request", "{\"error\":\"Debes enviar a y b\"}");
	}

	double a = 0.0;
	double b = 0.0;
	if (!parseDouble(aValue, a) || !parseDouble(bValue, b)) {
		return buildHttpResponse(400, "Bad Request", "{\"error\":\"a y b deben ser numeros\"}");
	}

	if (b == 0.0) {
		return buildHttpResponse(400, "Bad Request", "{\"error\":\"No se puede dividir entre cero\"}");
	}

	double result = a / b;
	if (!savePeticion(connection, a, b, result)) {
		return buildHttpResponse(500, "Internal Server Error", "{\"error\":\"No se pudo guardar la peticion en base de datos\"}");
	}

	std::ostringstream body;
	body << "{\"a\":" << a << ",\"b\":" << b << ",\"resultado\":" << result << "}";
	return buildHttpResponse(200, "OK", body.str());
}

}  // namespace

int main() {
	SQLHENV dbEnv = SQL_NULL_HENV;
	SQLHDBC dbConnection = SQL_NULL_HDBC;
	if (!initDatabaseConnection(dbEnv, dbConnection)) {
		std::cerr << "No se pudo conectar a la base de datos con autenticacion de Windows. Verifica servidor, driver ODBC y base divison." << std::endl;
		return 1;
	}

	WSADATA wsaData;
	int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (wsaResult != 0) {
		std::cerr << "Error al iniciar WinSock: " << wsaResult << std::endl;
		SQLDisconnect(dbConnection);
		SQLFreeHandle(SQL_HANDLE_DBC, dbConnection);
		SQLFreeHandle(SQL_HANDLE_ENV, dbEnv);
		return 1;
	}

	SOCKET serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (serverSocket == INVALID_SOCKET) {
		std::cerr << "No se pudo crear el socket." << std::endl;
		WSACleanup();
		SQLDisconnect(dbConnection);
		SQLFreeHandle(SQL_HANDLE_DBC, dbConnection);
		SQLFreeHandle(SQL_HANDLE_ENV, dbEnv);
		return 1;
	}

	sockaddr_in serverAddr{};
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(8080);

	if (bind(serverSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
		std::cerr << "No se pudo hacer bind al puerto 8080." << std::endl;
		closesocket(serverSocket);
		WSACleanup();
		SQLDisconnect(dbConnection);
		SQLFreeHandle(SQL_HANDLE_DBC, dbConnection);
		SQLFreeHandle(SQL_HANDLE_ENV, dbEnv);
		return 1;
	}

	if (listen(serverSocket, SOMAXCONN) == SOCKET_ERROR) {
		std::cerr << "No se pudo escuchar conexiones." << std::endl;
		closesocket(serverSocket);
		WSACleanup();
		SQLDisconnect(dbConnection);
		SQLFreeHandle(SQL_HANDLE_DBC, dbConnection);
		SQLFreeHandle(SQL_HANDLE_ENV, dbEnv);
		return 1;
	}

	std::cout << "API iniciada en http://localhost:8080" << std::endl;
	std::cout << "Ejemplo: http://localhost:8080/division?a=10&b=2" << std::endl;

	while (true) {
		SOCKET clientSocket = accept(serverSocket, nullptr, nullptr);
		if (clientSocket == INVALID_SOCKET) {
			std::cerr << "Error al aceptar cliente." << std::endl;
			continue;
		}

		char buffer[4096];
		int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
		if (bytesReceived > 0) {
			buffer[bytesReceived] = '\0';
			std::string request(buffer);
			std::string response = handleRequest(request, dbConnection);
			send(clientSocket, response.c_str(), static_cast<int>(response.size()), 0);
		}

		closesocket(clientSocket);
	}

	closesocket(serverSocket);
	WSACleanup();
	SQLDisconnect(dbConnection);
	SQLFreeHandle(SQL_HANDLE_DBC, dbConnection);
	SQLFreeHandle(SQL_HANDLE_ENV, dbEnv);
	return 0;
}
