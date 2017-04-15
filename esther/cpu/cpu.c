#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "commons/log.h"
#include "commons/config.h"
#include "commons/string.h"
#include <stdarg.h>

#define RUTA_CONFIG "config.cfg"
#define RUTA_LOG "cpu.log"

t_log* logger;
t_config* config;

enum CodigoDeOperacion {
	MENSAJE, CONSOLA, MEMORIA, FILESYSTEM, CPU
};

typedef struct headerDeLosRipeados {
	unsigned short bytesDePayload;
	char codigoDeOperacion; // 0 (mensaje). Handshake: 1 (consola), 2 (memoria), 3 (filesystem), 4 (cpu), 5 (kernel)
}__attribute__((packed, aligned(1))) headerDeLosRipeados;

int servidorKernel; //kernel
int servidorMemoria;
char IP_KERNEL[16]; // 255.255.255.255 = 15 caracteres + 1 ('\0')
int PUERTO_KERNEL;
char IP_MEMORIA[16];
int PUERTO_MEMORIA;

void desconectarConsola();
void leerMensaje();
void establecerConfiguracion();
void configurar(char*);
void conectarAKernel();
void handshake(int, char);
void serializarHeader(headerDeLosRipeados *, char *);
void deserializarHeader(headerDeLosRipeados *, char *);
void logearInfo(char *, ...);
void logearError(char *, int, ...);
void conectarAMemoria(void);

int main(void) {

	configurar("cpu");
	conectarAKernel();
	//conectarAMemoria();
	handshake(servidorKernel, CPU);
	//handshake(servidorMemoria, CPU);
	while (1){
		leerMensaje(servidorKernel);
		//leerMensaje(servidorMemoria);
		//watafaq? XD
	}

	return 0;
}


void desconectarConsola() {
	close(servidorKernel);
	//close(servidorMemoria);
	exit(EXIT_SUCCESS);
}

void leerMensaje(servidor) {

	int bytesRecibidos;
	char mensaje[512];
	bytesRecibidos = recv(servidor,&mensaje,sizeof(mensaje), 0);
	mensaje[bytesRecibidos]='\0';
	if(bytesRecibidos <= 0){
		close(servidor);
		logearError("Uno de los servidores fue desconectado luego de intentar leer mensaje", true);
	}
	logearInfo("Mensaje recibido: %s\n",mensaje);
}

void establecerConfiguracion() {
	if(config_has_property(config, "PUERTO_KERNEL")) {
		PUERTO_KERNEL = config_get_int_value(config, "PUERTO_KERNEL");
		logearInfo("Puerto Kernel: %d \n",PUERTO_KERNEL);
	} else {
		logearError("Error al leer el puerto del Kernel", true);
	}
	if(config_has_property(config, "IP_KERNEL")) {
		strcpy(IP_KERNEL,config_get_string_value(config, "IP_KERNEL"));
		logearInfo("IP Kernel: %s \n", IP_KERNEL);
	} else {
		logearError("Error al leer la IP del Kernel", true);
	}
	if(config_has_property(config, "PUERTO_MEMORIA")) {
		PUERTO_MEMORIA = config_get_int_value(config, "PUERTO_MEMORIA");
		logearInfo("Puerto Memoria: %d \n", PUERTO_MEMORIA);
	} else {
		logearError("Error al leer el puerto de la Memoria", true);
	}
	if(config_has_property(config, "IP_MEMORIA")){
		strcpy(IP_MEMORIA,config_get_string_value(config, "IP_MEMORIA"));
		logearInfo("IP Memoria: %s \n", IP_MEMORIA);
	} else {
		logearError("Error al leer la IP de la Memoria", true);
	}
}

int existeArchivo(const char *ruta)
{
    FILE *archivo;
    if ((archivo = fopen(ruta, "r")))
    {
        fclose(archivo);
        return true;
    }
    return false;
}

void conectarAKernel() {
	struct sockaddr_in direccionServidor;
	direccionServidor.sin_family = AF_INET;
	direccionServidor.sin_addr.s_addr = inet_addr( (char*) IP_KERNEL);
	direccionServidor.sin_port = htons(PUERTO_KERNEL);
	servidorKernel = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(servidorKernel, (struct sockaddr *) &direccionServidor,sizeof(direccionServidor)) < 0) {
		close(servidorKernel);
		logearError("No se pudo conectar al Kernel",true);
	}
	logearInfo("Conectado al Kernel\n");
}

void conectarAMemoria() {
	struct sockaddr_in direccionMemoria;
	direccionMemoria.sin_family = AF_INET;
	direccionMemoria.sin_addr.s_addr = inet_addr( (char*) IP_MEMORIA);
	direccionMemoria.sin_port = htons(PUERTO_MEMORIA);
	servidorMemoria = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(servidorMemoria, (struct sockaddr *) &direccionMemoria,sizeof(direccionMemoria)) < 0) {
		close(servidorMemoria);
		logearError("No se pudo conectar a la Memoria",true);
	}
	logearInfo("Conectado a la Memoria\n");
}

void configurar(char* quienSoy) {

	//Esto es por una cosa rara del Eclipse que ejecuta la aplicación
	//como si estuviese en la carpeta esther/consola/
	//En cambio, en la terminal se ejecuta desde esther/consola/Debug
	//pero en ese caso no existiria el archivo config ni el log
	//y es por eso que tenemos que leerlo desde el directorio anterior

	if (existeArchivo(RUTA_CONFIG)) {
		config = config_create(RUTA_CONFIG);
		logger = log_create(RUTA_LOG, quienSoy, false, LOG_LEVEL_INFO);
	}else{
		config = config_create(string_from_format("../%s",RUTA_CONFIG));
		logger = log_create(string_from_format("../%s",RUTA_LOG), quienSoy,false, LOG_LEVEL_INFO);
	}

	//Si la cantidad de valores establecidos en la configuración
	//es mayor a 0, entonces configurar la ip y el puerto,
	//sino, estaría mal hecho el config.cfg

	if(config_keys_amount(config) > 0) {
		establecerConfiguracion();
	} else {
		logearError("Error al leer archivo de configuración",true);
	}
	config_destroy(config);
}

void handshake(int socket, char operacion) {
	logearInfo("Conectando a servidor 0%%\n");
	headerDeLosRipeados handy;
	handy.bytesDePayload = 0;
	handy.codigoDeOperacion = operacion;

	int buffersize = sizeof(headerDeLosRipeados);
	char *buffer = malloc(buffersize);
	serializarHeader(&handy, buffer);
	send(socket, (void*) buffer, buffersize, 0);

	char respuesta[1024];
	int bytesRecibidos = recv(socket, (void *) &respuesta, sizeof(respuesta), 0);

	if (bytesRecibidos > 0) {
		logearInfo("Conectado a servidor 100 porciento\n");
		logearInfo("Mensaje del servidor: \"%s\"\n", respuesta);
	}
	else {
		logearError("Ripeaste\n",true);
	}
	free(buffer);
}

void serializarHeader(headerDeLosRipeados *header, char *buffer) {
	short *pBytesDePayload = (short*) buffer;
	*pBytesDePayload = header->bytesDePayload;
	char *pCodigoDeOperacion = (char*)(pBytesDePayload + 1);
	*pCodigoDeOperacion = header->codigoDeOperacion;
}

void deserializarHeader(headerDeLosRipeados *header, char *buffer) {
	short *pBytesDePayload = (short*) buffer;
	header->bytesDePayload = *pBytesDePayload;
	char *pCodigoDeOperacion = (char*)(pBytesDePayload + 1);
	header->codigoDeOperacion = *pCodigoDeOperacion;
}

void logearInfo(char* formato, ...) {
	char* mensaje;
	va_list args;
	va_start(args, formato);
	mensaje = string_from_vformat(formato,args);
	log_info(logger,mensaje);
	printf("%s", mensaje);
	va_end(args);
}

void logearError(char* formato, int terminar , ...) {
	char* mensaje;
	va_list args;
	va_start(args, terminar);
	mensaje = string_from_vformat(formato,args);
	log_error(logger,mensaje);
	printf("%s", mensaje);
	va_end(args);
	if (terminar==true) exit(0);
}
