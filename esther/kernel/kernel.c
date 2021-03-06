//-----HEADERS-----//
#include "op_memoria.h"
#include <unistd.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include "commons/collections/dictionary.h"
#include "commons/collections/queue.h"
#include <string.h>
#include "parser/metadata_program.h"
#include <sys/inotify.h>
#include "kernel.h"
#include "capafs.h"

//-----DEFINES-----//
#define ID_CLIENTE(x) ID_CLIENTES[x-CONSOLA]
#define DEF_MISMO_SOCKET(SOCKET)										\
		_Bool mismoSocket(void* elemento) {							\
			return SOCKET == ((miCliente *) elemento)->socketCliente;	\
		}
#define PUTO_INOTIFY "config/config.cfg"
enum Estado {NEW, READY, EXEC, BLOCKED, EXIT};
enum Algoritmo {RR, FIFO};

//Esto es para el inotify, no le den bola
#define EVENT_SIZE  ( sizeof (struct inotify_event) )
#define BUF_LEN     ( 1024 * ( EVENT_SIZE + 16 ) )

//-----ESTRUCTURAS-----//
typedef t_list listaCliente;
typedef t_list listaProcesos;
typedef struct datosMemoria {
	int pid;
	short int codeSize;
	char *code;
} PACKED datosMemoria;
typedef struct miCliente {
    short socketCliente;
    char identificador;
    Proceso *proceso_asociado;
    _Bool va_a_desconectarse;
} miCliente;
typedef struct Posicion_memoria {
	int numero_pagina;
	int offset;
	int size;
} Posicion_memoria;
typedef struct Variable {
	char identificador;
	Posicion_memoria posicion;
} PACKED Variable;
typedef struct Entrada_stack {
	t_list *args;
	t_list *vars;
	int retPos;
	Posicion_memoria retVar;
} Entrada_stack;
typedef struct Semaforo_QEPD {
	int valor;
	t_list *bloqueados;
} Semaforo_QEPD;

//-----VARIABLES GLOBALES-----//
t_log* logger;
t_config* config;
listaCliente *clientes;
t_list* lista_NEW;
listaProcesos *procesos; //Dentro van a estar los procesos en estado READY/EXEC/BLOCKED
t_list* lista_EXIT;

static const char *ID_CLIENTES[] = {"Consola", "Memoria", "File System", "CPU"};
int PUERTO_KERNEL;
int QUANTUM_VALUE;
int QUANTUM_SLEEP_VALUE;
char ALGORITMO[5];
int algoritmo_actual;
int GRADO_MULTIPROG;
t_dictionary *semaforos;
t_dictionary *variables_compartidas;
int PID_GLOBAL;
_Bool planificacion_activa = true;
int descriptor_cambios_archivo;

/* op_memoria.h */
int socket_memoria;
int tamanio_pagina;
char IP_MEMORIA[16];
int PUERTO_MEMORIA;
int STACK_SIZE;

/* capafs.h */
char IP_FS[16];
int PUERTO_FS;
global_file_table tabla_archivos_global;

//-----PROTOTIPOS DE FUNCIONES-----//
char*		remover_salto_linea(char*);

inline int marco_size(void);
inline void imprimir_opciones_kernel();
inline void limpiar_buffer_entrada();

int			algoritmo_actual_es(char *);
int 		cantidad_procesos_sistema();
int 		existe_cliente(int);
int			existe_proceso(int);
int 		recibir_handshake(int);
int			solo_numeros(char*);
int 		tipo_cliente(int);

miCliente*	algun_CPU_disponible();

Proceso*	algun_proceso_listo();
Proceso* 	proceso_segun_cpu(int);
Proceso* 	proceso_segun_pid(int);

PCB*		deserializar_PCB(void*);

void 		agregar_cliente(char, int);
void 		agregar_proceso(Proceso *);
void 		borrar_cliente(int);
void 		cerrar_conexion(int, char*);
void		destruir_PCB(PCB*);
void		eliminar_proceso(int);
void 		enviar_excepcion(int, int);
void 		establecer_configuracion();
void 		finalizar_programa(int);
void 		hacer_pedido_memoria(datosMemoria);
void		inicializar_proceso(int, char *, Proceso *);
void 		intentar_iniciar_proceso();
void		intentar_desbloquear_proceso(char*);
void 		interaccion_kernel();
void		limpiar_proceso(Proceso*);
void		listar_procesos_en_estado();
void		remover_de_semaforos(int);
void 		peticion_para_cerrar_proceso(int, int);
void		planificar();
void 		procesar_mensaje(int, char, int);
void 		procesar_operaciones_consola(int, char, int);
void 		procesar_operaciones_CPU(int, char, int);
void*		serializar_PCB(PCB*, int*);
void		terminar_kernel();

bool eliminar_pagina_heap(t_list*, int);
void destruir_lista_paginas_heap(Proceso*);

//-----PROCEDIMIENTO PRINCIPAL-----//
int main(void) {
	semaforos = dictionary_create();
	variables_compartidas = dictionary_create();

	if (existe_archivo(PUTO_INOTIFY)) {
		config = config_create(PUTO_INOTIFY);

		char *ruta = strdup("kernel_");
		char *timestamp = obtener_timestamp();

		string_append(&ruta, timestamp);
		string_append(&ruta, ".log");

		free(timestamp);

		logger = log_create(ruta, "kernel", false, LOG_LEVEL_INFO);
		free(ruta);
	}
	else {
		logear_error("No existe el archivo de configuración", true);
	}

	if (config_keys_amount(config) > 0) {
		establecer_configuracion();
	}
	else {
		logear_error("Error al leer archivo de configuración", true);
	}

	config_destroy(config);

	clientes = list_create();

	lista_NEW = list_create();
	procesos = list_create();
	lista_EXIT = list_create();

	init_tabla_archivos();

	int servidor = socket(AF_INET, SOCK_STREAM, 0);

	if (servidor == -1) {
		logear_error("No se pudo crear el socket", true);
	}

	int activado = 1;
	setsockopt(servidor, SOL_SOCKET, SO_REUSEADDR, &activado, sizeof(activado));

	struct sockaddr_in servidor_info;

	servidor_info.sin_family = AF_INET;
	servidor_info.sin_port = htons(PUERTO_KERNEL);
	servidor_info.sin_addr.s_addr = INADDR_ANY;
	bzero(&(servidor_info.sin_zero), 8);

	if (bind(servidor, (struct sockaddr*) &servidor_info, sizeof(struct sockaddr)) == -1) {
		logear_error("Fallo al bindear el puerto", true);
	}

    if (listen(servidor, 10) == -1) {
		logear_error("Fallo al escuchar", true);
    }

    logear_info("Estoy escuchando");

    fd_set conectados;		// Set de FDs conectados
    fd_set read_fds;		// sockets de lectura

    FD_ZERO(&conectados);
    FD_ZERO(&read_fds);

    int fdmax;				// valor maximo de los FDs

    mem_conectar();

	agregar_cliente(MEMORIA, socket_memoria);

	int socket_fs = fs_conectar();
	if (socket_fs == -1) {
    	logear_error("No se pudo conectar al file system.", true);
	}
	agregar_cliente(FILESYSTEM, socket_fs);

	descriptor_cambios_archivo = inotify_init();

	fdmax = (socket_memoria > servidor) ? socket_memoria : servidor;

	if (descriptor_cambios_archivo < 0 ) {
		logear_error("No se puede detectar los cambios en el directorio", false);
	} else {
		fdmax = (descriptor_cambios_archivo > servidor) ? descriptor_cambios_archivo : servidor;
		inotify_add_watch(descriptor_cambios_archivo,"config", IN_CLOSE_WRITE);
	    FD_SET(descriptor_cambios_archivo, &conectados);
	}

    FD_SET(servidor, &conectados);
    FD_SET(socket_memoria, &conectados);
    FD_SET(socket_fs, &conectados);
    FD_SET(fileno(stdin), &conectados);

    imprimir_opciones_kernel();

    for(;;) {
        read_fds = conectados;

		if (select(fdmax + 1, &read_fds, NULL, NULL, NULL) == -1) {
			logear_error("Error en el select", true);
        }

        struct sockaddr_in cliente_info;

        int i;
        for(i = 0; i <= fdmax; i++) {
        	if (FD_ISSET(i, &read_fds) == 0) { // No hubo cambios en este socket
        		continue;
        	}

        	// Se detecta un cambio en el socket de escucha => hay un nuevo cliente
			if (i == servidor) {
		        int nuevoCliente;			// Socket del nuevo cliente conectado

		        socklen_t addrlen = sizeof cliente_info;
				nuevoCliente = accept(servidor, (struct sockaddr*) &cliente_info, &addrlen);

				if (nuevoCliente == -1) {
					logear_error("Fallo en el accept", false);
				}
				else {
					FD_SET(nuevoCliente, &conectados);		// Agregarlo al set
					if (nuevoCliente > fdmax) {
						fdmax = nuevoCliente;				// Cambia el maximo
					}
					logear_info("Nueva conexión desde %s en el socket %d",
							inet_ntoa(cliente_info.sin_addr), nuevoCliente);
				}
			}
			// Mensaje por interfaz del Kernel
			else if (i == fileno(stdin)) {
				interaccion_kernel();
			}

			else if (i == descriptor_cambios_archivo) {
				char buffer_cambios_archivo[BUF_LEN];
				read(descriptor_cambios_archivo, buffer_cambios_archivo, BUF_LEN);
				usleep(1000000);
				//Realmente es muy raro lo que pasaba acá, parece que el INOTIFY
				//avisaba justo cuando empezaba a guardarse el archivo,
				//Entonces, al crear la config., el archivo todavía se estaba
				//guardando y por eso había seg faults.
				t_config *nueva_config = config_create(PUTO_INOTIFY);
				int NEW_QUANTUM_SLEEP = config_get_int_value(nueva_config, "QUANTUM_SLEEP");
				if (NEW_QUANTUM_SLEEP != QUANTUM_SLEEP_VALUE) {
					QUANTUM_SLEEP_VALUE = NEW_QUANTUM_SLEEP;
					printf("Nuevo QUANTUM_SLEEP: %d\n", QUANTUM_SLEEP_VALUE);
				}
				config_destroy(nueva_config);
			}

			// Un cliente mando un mensaje
			else {
				if (existe_cliente(i) == 0) {			// Nuevo cliente, debe enviar un handshake
					if (recibir_handshake(i) == 0) {
						cerrar_conexion(i, "El socket %d se desconectó");
						FD_CLR(i, &conectados);
					}
					else {
						char *respuesta = "Bienvenido!";
						send(i, respuesta, strlen(respuesta) + 1, 0);
						if (tipo_cliente(i) == CPU){

							enviar_header(i, ALGORITMO_ACTUAL, sizeof(algoritmo_actual));
							send(i, &algoritmo_actual, sizeof(algoritmo_actual), 0);
							if (algoritmo_actual_es("RR")) {
								enviar_header(i, QUANTUM, sizeof(QUANTUM_VALUE));
								send(i, &QUANTUM_VALUE, sizeof(QUANTUM_VALUE), 0);
							}
							enviar_header(i, PAGINAS_STACK, sizeof(STACK_SIZE));
							send(i, &STACK_SIZE, sizeof(STACK_SIZE), 0);
							planificar();
						}
					}
				}
				else {
					//Recibir header
					headerDeLosRipeados header;
					int bytesRecibidos = recibir_header(i, &header);
					if (bytesRecibidos <= 0) {
						int tipo = tipo_cliente(i);
						if (tipo >= 0) {
							logear_info("Cliente [%s] desconectado", ID_CLIENTE(tipo));
							if (tipo == CPU) {
								Proceso *proceso = proceso_segun_cpu(i);
								if (proceso != NULL) {
									logear_info("Se desconectó la CPU que ejecutaba (PID:%d), finalizando proceso..", proceso->pcb->pid);
									proceso->pcb->exit_code = DESCONEXION_FORZADA_CPU;
									finalizar_programa(proceso->pcb->pid);
								}
							}
							borrar_cliente(i);
							close(i);
							if (tipo == CONSOLA) {
								logear_info("La consola (Socket:%d) se desconectó, sus procesos serán finalizados", i);
								void pedir_finalizacion(void *param) {
									Proceso *proceso = param;
									if (proceso->consola == i) {
										proceso->pcb->exit_code = DESCONEXION_CONSOLA;
										finalizar_programa(proceso->pcb->pid);
										//peticion_para_cerrar_proceso(proceso->pcb->pid, DESCONEXION_CONSOLA);
									}
								}
								list_iterate(procesos, &pedir_finalizacion);
							}
							if (tipo == MEMORIA || tipo == FILESYSTEM)
								terminar_kernel();
						} else {
							cerrar_conexion(i, "El socket %d se desconectó");
						}
						FD_CLR(i, &conectados);
						continue;
					}

					int bytesDePayload = header.bytesDePayload;
					int codigoDeOperacion = header.codigoDeOperacion;

					//Procesar operación del header
					procesar_mensaje(i,codigoDeOperacion,bytesDePayload);
				}
			}
        }
    }
    list_destroy_and_destroy_elements(clientes, free);
    return 0;
}

//-----DEFINICIÓN DE FUNCIONES-----//

//MENSAJES
void enviar_excepcion(int socket_cliente, int excepcion) {
	enviar_header(socket_cliente, EXCEPCION, sizeof(excepcion));
	send(socket_cliente, &excepcion, sizeof(excepcion), MSG_NOSIGNAL);
}
void procesar_mensaje(int socket_cliente, char operacion, int bytes) {

	int tipo = tipo_cliente(socket_cliente);

	switch(tipo) {
		case CONSOLA:
			procesar_operaciones_consola(socket_cliente, operacion, bytes);
			break;

		case CPU:
			procesar_operaciones_CPU(socket_cliente, operacion, bytes);
			break;

		default:
			logear_error("Operación inválida de %s", false, ID_CLIENTE(tipo));
			break;
	}
}
void procesar_operaciones_consola(int socket_cliente, char operacion, int bytes) {
	switch (operacion) {

	case INICIAR_PROGRAMA:;
		char* codigo = malloc(bytes+1);
		recv(socket_cliente, codigo, bytes, MSG_WAITALL);
		codigo[bytes]='\0';

		Proceso* nuevo_proceso = malloc(sizeof(Proceso));
		inicializar_proceso(socket_cliente, codigo, nuevo_proceso);

		list_add(lista_NEW, nuevo_proceso);
		logear_info("Petición de inicio de proceso [PID:%d]", PID_GLOBAL);
		PID_GLOBAL++;

		intentar_iniciar_proceso();
		break;

	case FINALIZAR_PROGRAMA:;
		int PID;
		recv(socket_cliente, &PID, sizeof(PID), 0);
		Proceso *proceso = proceso_segun_pid(PID);
		if (proceso != NULL) {
			proceso->pcb->exit_code = COMANDO_FINALIZAR_PROGRAMA;
			finalizar_programa(PID);
		} else {
			logear_info("Error al intentar finalizar (PID:%d). Proceso inexistente.", PID);
		}
		break;

	case DESCONECTAR_CONSOLA:;
		_Bool mismo_cliente(void *p) {
			return socket_cliente == ((miCliente*)p)->socketCliente;
		}
		miCliente *consola = list_find(clientes, &mismo_cliente);
		consola->va_a_desconectarse = true;
		_Bool misma_consola(void *p) {
			Proceso *pr = p;
			return pr->consola == socket_cliente;
		}
		void destruir_entrada_stack(void *param) {
			Entrada_stack *entrada = (Entrada_stack *) param;
			list_destroy_and_destroy_elements(entrada->vars, free);
			list_destroy_and_destroy_elements(entrada->args, free);
			free(entrada);
		}
		while (list_any_satisfy(lista_NEW, &misma_consola)) {
			Proceso *proceso_aux = list_remove_by_condition(lista_NEW, &misma_consola);
			proceso_aux->pcb->exit_code = DESCONEXION_CONSOLA;
			logear_info("[PID:%d] Finalizó con EXIT_CODE:%d", proceso_aux->pcb->pid, proceso_aux->pcb->exit_code);
			list_destroy(proceso_aux->paginas_heap);
			list_destroy(proceso_aux->pcb->tabla_archivos);
			list_destroy_and_destroy_elements(proceso_aux->pcb->indice_stack, &destruir_entrada_stack);
			free(proceso_aux->pcb->etiquetas);
			free(proceso_aux->pcb->instrucciones_serializado);
			list_add(lista_EXIT, proceso_aux);
		}
		while (list_any_satisfy(procesos, &misma_consola)) {
			Proceso *proceso_aux = list_find(procesos, &misma_consola);
			proceso_aux->pcb->exit_code = DESCONEXION_CONSOLA;
			finalizar_programa(proceso_aux->pcb->pid);
		}
		consola->va_a_desconectarse = false;

		break;
	}
}
void procesar_operaciones_CPU(int socket_cliente, char operacion, int bytes) {
	int pid, excepcion, respuesta;
	char *nombre = NULL;
	int *valor = NULL;
	Proceso *proceso = proceso_segun_cpu(socket_cliente);
	Semaforo_QEPD *semaforo;
	int *id_proceso = NULL;
	t_descriptor_archivo descriptor;

	_Bool mismo_cliente(void *param) {
		miCliente *cliente = param;
		return cliente->socketCliente == socket_cliente;
	}

	switch (operacion) {

	case PCB_INCOMPLETO:
		pid = actualizar_PCB(socket_cliente, bytes);

		if (proceso->estado == EXIT) {
			logear_info("[PID:%d] Finalización por petición de consola", pid);
			finalizar_programa(pid);
		} else {
			proceso->estado = READY;
			planificar();
		}
		break;

	case PCB_BLOQUEADO:
		pid = actualizar_PCB(socket_cliente, bytes);

		if (proceso->estado == EXIT) {
			logear_info("[PID:%d] Finalización por petición de consola", pid);
			finalizar_programa(pid);
		} else {
			proceso->estado = BLOCKED;
			planificar();
		}
		break;

	case PCB_COMPLETO:
		pid = actualizar_PCB(socket_cliente, bytes);
		finalizar_programa(pid);
		break;

	case PCB_EXCEPCION:
		pid = actualizar_PCB(socket_cliente, bytes);
		finalizar_programa(pid);
		break;

	case DESCONEXION_CPU:;
		miCliente *cpu = list_find(clientes, &mismo_cliente);
		if (cpu != NULL) {
			cpu->va_a_desconectarse = true;
		}
		break;

	case OBTENER_VALOR_VARIABLE:
		proceso->cantidad_syscalls++;

		nombre = malloc(bytes);
		recv(socket_cliente, nombre, bytes, MSG_WAITALL);

		if (dictionary_has_key(variables_compartidas, nombre))
			valor = dictionary_get(variables_compartidas, nombre);

		if (valor == NULL) {
			enviar_excepcion(socket_cliente, VARIABLE_COMPARTIDA_INEXISTENTE);
		} else {
			enviar_header(socket_cliente, OBTENER_VALOR_VARIABLE, sizeof(int));
			send(socket_cliente, valor, sizeof(int), MSG_NOSIGNAL);
		}
		free(nombre);
		break;

	case ASIGNAR_VALOR_VARIABLE:
		proceso->cantidad_syscalls++;

		nombre = malloc(bytes);
		recv(socket_cliente, nombre, bytes, MSG_WAITALL);
		int *valor_nuevo = malloc(sizeof(int));
		recv(socket_cliente, valor_nuevo, sizeof(int), MSG_WAITALL);

		if (dictionary_has_key(variables_compartidas, nombre))
			valor = dictionary_get(variables_compartidas, nombre);

		if (valor == NULL) {
			enviar_excepcion(socket_cliente, VARIABLE_COMPARTIDA_INEXISTENTE);
		} else {
			memcpy(valor, valor_nuevo, sizeof(int));
			enviar_header(socket_cliente, PETICION_CORRECTA, 0);
			logear_info("Se cambió el valor de la variable compartida !%s a %d", nombre, *valor_nuevo);
		}
		free(valor_nuevo);
		free(nombre);
		break;

	case WAIT:
		proceso->cantidad_syscalls++;

		nombre = malloc(bytes);
		recv(socket_cliente, nombre, bytes, MSG_WAITALL);
		semaforo = NULL;

		if (dictionary_has_key(semaforos, nombre))
			semaforo = dictionary_get(semaforos, nombre);

		if (semaforo == NULL) {
			enviar_excepcion(socket_cliente, SEMAFORO_INEXISTENTE);
		} else {
			if (semaforo->valor > 0) {
				semaforo->valor--;
				logear_info("Semáforo %s cambia su valor a %d debido al proceso (PID:%d)", nombre, semaforo->valor, proceso->pcb->pid);
				enviar_header(socket_cliente, PETICION_CORRECTA, 0);
			} else {
				enviar_header(socket_cliente, BLOQUEAR, 0);
				list_add(semaforo->bloqueados, proceso);
			}
		}
		free(nombre);
		break;

	case SIGNAL:
		proceso->cantidad_syscalls++;

		nombre = malloc(bytes);
		recv(socket_cliente, nombre, bytes, MSG_WAITALL);
		semaforo = NULL;

		if (dictionary_has_key(semaforos, nombre))
			semaforo = dictionary_get(semaforos, nombre);

		if (semaforo == NULL) {
			enviar_excepcion(socket_cliente, SEMAFORO_INEXISTENTE);
		} else {
			if (list_size(semaforo->bloqueados) > 0) {
				intentar_desbloquear_proceso(nombre);
			} else {
				semaforo->valor++;
				logear_info("Semaforo %s cambia su valor a %d debido al proceso (PID:%d)", nombre, semaforo->valor, proceso->pcb->pid);
			}
			enviar_header(socket_cliente, PETICION_CORRECTA, 0);
		}
		free(nombre);
		break;

	case ESCRIBIR_ARCHIVO:
		proceso->cantidad_syscalls++;

		void *informacion = malloc(bytes);
		recv(socket_cliente, informacion, bytes, MSG_WAITALL);
		recv(socket_cliente, &descriptor, sizeof(t_descriptor_archivo), MSG_WAITALL);

		if (proceso_segun_pid(proceso->pcb->pid) == NULL) {
			enviar_excepcion(socket_cliente, EXCEPCION_KERNEL);
			return;
		}

		if (descriptor == 1) {
			if (!list_any_satisfy(clientes, &mismo_cliente)) {
				logear_error("[PID:%d] Intentó enviar mensaje a una consola desconectada", false, proceso->pcb->pid);
				enviar_excepcion(socket_cliente, DESCONEXION_CONSOLA);
				return;
			}

			enviar_header(proceso->consola, IMPRIMIR, bytes);
			send(proceso->consola, &proceso->pcb->pid, sizeof(int), MSG_NOSIGNAL);
			send(proceso->consola, informacion, bytes, MSG_NOSIGNAL);
			//logear_info("[PID:%d] Se mandó a imprimir un texto a la consola", proceso->pcb->pid);
			enviar_header(socket_cliente, PETICION_CORRECTA, 0);
		} else {
			respuesta = fs_escribir_archivo(proceso->pcb->pid, descriptor, informacion, bytes);
			if (respuesta < 0) {
				enviar_excepcion(socket_cliente, respuesta);
			} else {
				enviar_header(socket_cliente, PETICION_CORRECTA, 0);
			}
		}
		free(informacion);
		break;

	case LEER_ARCHIVO:
		proceso->cantidad_syscalls++;

		Posicion_memoria pedido;
		recv(socket_cliente, &pedido, bytes, MSG_WAITALL);
		recv(socket_cliente, &descriptor, sizeof(t_descriptor_archivo), MSG_WAITALL);

		if (proceso_segun_pid(proceso->pcb->pid) == NULL) {
			enviar_excepcion(socket_cliente, EXCEPCION_KERNEL);
			return;
		}

		void *info = fs_leer_archivo(proceso->pcb->pid, descriptor, pedido.size, &respuesta);
		if (info != NULL) {
			char *texto = info;
			logear_info("[PID:%d] Lee '%.*s' de %d bytes de un archivo. Almacenando en memoria...", proceso->pcb->pid, pedido.size, texto, pedido.size);
			respuesta = mem_escribir_bytes(proceso->pcb->pid, pedido.numero_pagina, pedido.offset, pedido.size, info);
			if (respuesta < 0) {
				enviar_excepcion(socket_cliente, respuesta);
			} else {
				enviar_header(socket_cliente, PETICION_CORRECTA, 0);
			}
			free(info);
		} else {
			enviar_excepcion(socket_cliente, respuesta);
		}

		break;

	case ABRIR_ARCHIVO:
		proceso->cantidad_syscalls++;

		t_direccion_archivo direccion = malloc(bytes);
		recv(socket_cliente, direccion, bytes, MSG_WAITALL);
		flags_t flags;
		recv(socket_cliente, &flags, sizeof(flags_t), MSG_WAITALL);

		if (proceso_segun_pid(proceso->pcb->pid) == NULL) {
			enviar_excepcion(socket_cliente, EXCEPCION_KERNEL);
			return;
		}

		respuesta = fs_abrir_archivo(proceso->pcb->pid, direccion, flags);
		if (respuesta < 0) {
			enviar_excepcion(socket_cliente, respuesta);
		} else {
			enviar_header(socket_cliente, ABRIR_ARCHIVO, sizeof(int));
			send(socket_cliente, &respuesta, sizeof(int), MSG_NOSIGNAL);
		}
		free(direccion);
		break;

	case BORRAR_ARCHIVO:
		proceso->cantidad_syscalls++;

		recv(socket_cliente, &descriptor, sizeof(t_descriptor_archivo), MSG_WAITALL);

		if (proceso_segun_pid(proceso->pcb->pid) == NULL) {
			enviar_excepcion(socket_cliente, EXCEPCION_KERNEL);
			return;
		}

		respuesta = fs_borrar_archivo(proceso->pcb->pid, descriptor);
		if (respuesta < 0) {
			enviar_excepcion(socket_cliente, respuesta);
		} else {
			enviar_header(socket_cliente, PETICION_CORRECTA, 0);
		}
		break;

	case CERRAR_ARCHIVO:
		proceso->cantidad_syscalls++;

		recv(socket_cliente, &descriptor, sizeof(t_descriptor_archivo), MSG_WAITALL);

		if (proceso_segun_pid(proceso->pcb->pid) == NULL) {
			enviar_excepcion(socket_cliente, EXCEPCION_KERNEL);
			return;
		}

		respuesta = fs_cerrar_archivo(proceso->pcb->pid, descriptor);
		if (respuesta < 0) {
			enviar_excepcion(socket_cliente, respuesta);
		} else {
			enviar_header(socket_cliente, PETICION_CORRECTA, 0);
		}

		break;

	case MOVER_CURSOR:
		proceso->cantidad_syscalls++;

		recv(socket_cliente, &descriptor, sizeof(t_descriptor_archivo), MSG_WAITALL);
		t_valor_variable posicion;
		recv(socket_cliente, &posicion, sizeof(t_valor_variable), MSG_WAITALL);

		if (proceso_segun_pid(proceso->pcb->pid) == NULL) {
			enviar_excepcion(socket_cliente, EXCEPCION_KERNEL);
			return;
		}

		respuesta = fs_mover_cursor(proceso->pcb->pid, descriptor, posicion);
		if (respuesta < 0) {
			enviar_excepcion(socket_cliente, respuesta);
		} else {
			enviar_header(socket_cliente, PETICION_CORRECTA, 0);
		}

		break;

	case RESERVAR:
		proceso->cantidad_syscalls++;

		int bytes_pedidos;
		recv(socket_cliente, &bytes_pedidos, bytes, MSG_WAITALL);

		if (proceso_segun_pid(proceso->pcb->pid) == NULL) {
			enviar_excepcion(socket_cliente, EXCEPCION_KERNEL);
			return;
		}

		respuesta = alocar_bloque(proceso->pcb->pid, bytes_pedidos);

		if (respuesta < 0) {
			enviar_excepcion(socket_cliente, respuesta);
		} else {
			proceso->cantidad_alocar++;
			proceso->bytes_alocados = proceso->bytes_alocados + bytes_pedidos;
			enviar_header(socket_cliente, RESERVAR, sizeof(t_puntero));
			send(socket_cliente, &respuesta, sizeof(t_puntero), MSG_NOSIGNAL);
		}

		break;

	case LIBERAR:
		proceso->cantidad_syscalls++;

		t_puntero puntero;
		recv(socket_cliente, &puntero, bytes, MSG_WAITALL);

		if (proceso_segun_pid(proceso->pcb->pid) == NULL) {
			enviar_excepcion(socket_cliente, EXCEPCION_KERNEL);
			return;
		}

		respuesta = liberar_bloque_pro(proceso->pcb->pid, puntero);

		if (respuesta < 0) {
			enviar_excepcion(socket_cliente, respuesta);
		} else {
			proceso->cantidad_liberar++;
			enviar_header(socket_cliente, PETICION_CORRECTA, 0);
		}

		break;

	}
}
int recibir_handshake(int socketCliente) {
	headerDeLosRipeados handy;

	recibir_header(socketCliente, &handy);

	if (CONSOLA <= handy.codigoDeOperacion
			&& handy.codigoDeOperacion <= CPU
			&& handy.bytesDePayload == 0) {
		logear_info("El nuevo cliente fue identificado como: %s", ID_CLIENTE(handy.codigoDeOperacion));
		agregar_cliente(handy.codigoDeOperacion, socketCliente);
		return 1;
	}
	return 0;
}

//INTERACCIÓN POR CONSOLA//
inline void imprimir_opciones_kernel() {
	printf(
			"\n--------------------\n"
			"BIENVENIDO AL KERNEL\n"
			"Lista de comandos: \n"
			"listado"
				"\t\t\t\t\t//Ver procesos de todas las colas\n"
			"listado [NEW/READY/EXEC/BLOCKED/EXIT]"
				"\t//Ver procesos en una cola específica\n"
			"proceso [Número de PID]"
				"\t\t\t//Ver información del proceso PID\n"
			"tablaglobal"
				"\t\t\t\t//Ver la tabla global de archivos\n"
			"multiprogramacion [Número de grado]"
				"\t//Cambiar el grado de multiprogramación\n"
			"finalizar [Número de PID]"
				"\t\t//Finaliza el proceso PID\n"
			"detener"
				"\t\t\t\t\t//Detiene la planificación\n"
			"planificar"
				"\t\t\t\t//Inicia la planificación\n"
			"opciones"
				"\t\t\t\t//Mostrar opciones\n"
	);
}
void interaccion_kernel() {
	struct comando {
		char *nombre;
		void (*funcion) (char *param);
	};

	void listado(char *estado) {
		logear_info("Comando de listado de programas ejecutado");
		string_trim(&estado);
		if (strlen(estado) == 0) {
			logear_info("[Listado - Todos los estados]");
			listar_procesos_en_estado(NEW);
			listar_procesos_en_estado(READY);
			listar_procesos_en_estado(EXEC);
			listar_procesos_en_estado(BLOCKED);
			listar_procesos_en_estado(EXIT);
		} else if (!strcmp("NEW",estado)) {
			logear_info("[Listado - Procesos en estado nuevo]");
			listar_procesos_en_estado(NEW);
		} else if (!strcmp("READY",estado)) {
			logear_info("[Listado - Procesos en estado listo]");
			listar_procesos_en_estado(READY);
		} else if (!strcmp("EXEC",estado)) {
			logear_info("[Listado - Procesos en estado de ejecucion]");
			listar_procesos_en_estado(EXEC);
		} else if (!strcmp("BLOCKED",estado)) {
			logear_info("[Listado - Procesos en estado bloqueado]");
			listar_procesos_en_estado(BLOCKED);
		} else if (!strcmp("EXIT",estado)) {
			logear_info("[Listado - Procesos en estado terminado]");
			listar_procesos_en_estado(EXIT);
		} else {
			logear_error("[Listado] Parámetro desconocido", false);
		}
		free(estado);
	}

	void proceso(char *sPID) {
		string_trim(&sPID);

		if (strlen(sPID) == 0) {
			logear_error("El comando \"proceso\" recibe un parametro [PID]", false);
			free(sPID);
			return;
		}

		if (!solo_numeros(sPID)) {
			logear_error("Error: \"%s\" no es un PID valido!", false, sPID);
			free(sPID);
			return;
		}

		int PID = strtol(sPID, NULL, 0);
		free(sPID);

		Proceso *proceso = proceso_segun_pid(PID);

		if (proceso != NULL) {
			logear_info("[Proceso %d]", PID);
		} else {
			_Bool mismo_pid(void *param) {
				return ((Proceso*)param)->pcb->pid == PID;
			}
			proceso = list_find(lista_EXIT, &mismo_pid);
			if (proceso != NULL) {
				logear_info("[Proceso %d - Finalizado]", PID);
			} else {
				logear_info("[Proceso no existe o no inició]");
				return;
			}
		}

		logear_info("(Cantidad de ráfagas: %d)", proceso->cantidad_rafagas);
		logear_info("(Cantidad de syscalls: %d)", proceso->cantidad_syscalls);
		if (proceso->estado != EXIT) {
			file_descriptor_t fd = FD_INICIAL;
			void imprimir_info_pft(void *param) {
				info_pft *info = param;
				if (info == NULL) return;
				logear_info("(Archivo FD:%d) (FD_global:%d) (Cursor:%d) (L:%d) (E:%d) (C:%d)", fd, info->fd_global, info->posicion, info->banderas.lectura, info->banderas.escritura, info->banderas.creacion);
				fd++;
			}
			list_iterate(proceso->pcb->tabla_archivos, &imprimir_info_pft);
		} else {
			logear_info("(Exit code: %d)", proceso->pcb->exit_code);
		}
		logear_info("(Cantidad de páginas de Heap: %d)", proceso->cantidad_paginas_heap);
		logear_info("(Cantidad de alocar: %d) (Bytes reservados: %d)", proceso->cantidad_alocar, proceso->bytes_alocados);
		logear_info("(Cantidad de liberar: %d) (Bytes liberados: %d)", proceso->cantidad_liberar, proceso->bytes_liberados);
	}

	void tablaglobal(char *param) {
		free(param);
		logear_info("[Tabla Global]");
		void imprimir_info_gft(void *param) {
			info_gft *info = param;
			if (info == NULL) return;
			logear_info("[Archivo] Ruta:%s Cantidad:%d", info->path, info->cantidad);
		}
		if (list_size(tabla_archivos_global) > 0) {
			list_iterate(tabla_archivos_global, &imprimir_info_gft);
		} else {
			logear_info("[Ningún archivo abierto]");
		}
	}

	void multiprogramacion(char *param) {
		int nuevo_grado = atoi(param);
		free(param);
		if (nuevo_grado > 0) {
			GRADO_MULTIPROG = nuevo_grado;
			logear_info("[Nuevo grado de multiprogramacion: %d]", GRADO_MULTIPROG);
			intentar_iniciar_proceso();
		} else {
			logear_info("[Grado de multiprogramacion inválido]");
		}

	}

	void finalizar(char *sPID) {
		logear_info("Comando de desconexión de programa ejecutado");

		string_trim(&sPID);

		if (strlen(sPID) == 0) {
			logear_error("El comando \"finalizar\" recibe un parametro [PID]", false);
			free(sPID);
			return;
		}

		if (!solo_numeros(sPID)) {
			logear_error("Error: \"%s\" no es un PID valido!", false, sPID);
			free(sPID);
			return;
		}

		int PID = strtol(sPID, NULL, 0);
		free(sPID);

		peticion_para_cerrar_proceso(PID, COMANDO_FINALIZAR_PROGRAMA);
	}

	void detener(char *param) {
		free(param);
		if (planificacion_activa) {
			planificacion_activa = false;
			logear_info("[Planificacion detenida]");
		} else {
			logear_info("[La planificacion ya está desactivada]");
		}
	}

	void activar_planificacion(char *param) {
		free(param);
		if (!planificacion_activa) {
			planificacion_activa = true;
			logear_info("[Planificacion activada]");
			planificar();
		} else {
			logear_info("[La planificacion ya está activada]");
		}
	}

	void opciones(char *param) {
		free(param);
		imprimir_opciones_kernel();
	}

	struct comando comandos[] = {
		{ "listado", listado },
		{ "proceso", proceso },
		{ "tablaglobal", tablaglobal },
		{ "multiprogramacion", multiprogramacion },
		{ "finalizar", finalizar },
		{ "detener", detener },
		{ "planificar", activar_planificacion },
		{ "opciones" , opciones }
	};

	char input[100];
	memset(input, 0, sizeof input);
	fgets(input, sizeof input, stdin);

	if (strlen(input) == 1) {
		return;
	}

	if (input[strlen(input) - 1] != '\n') {
		logear_error("Un comando no puede tener mas de 100 caracteres", false);
		limpiar_buffer_entrada();
		return;
	}

	remover_salto_linea(input);

	char *inputline = strdup(input); // Si no hago eso, string_trim se rompe
	string_trim_left(&inputline); // Elimino espacios a la izquierda

	char *cmd = NULL; // Comando
	char *save = NULL; // Apunta al primer caracter de los siguentes caracteres sin parsear
	char *delim = " ,"; // Separador

	// strtok_r(3) devuelve un token leido
	cmd = strtok_r(inputline, delim, &save); // Comando va a ser el primer token

	int i;
	// La division de los sizeof me calcula la cantidad de comandos
	for (i = 0; i < (sizeof comandos / sizeof *comandos); i++) {
		char *_cmd = comandos[i].nombre;
		if (strcmp(cmd, _cmd) == 0) {
			char *param = strdup(save); // Para no pasarle save por referencia
			comandos[i].funcion(param);
			break;
		}
	}
	if (i == (sizeof comandos / sizeof *comandos)) {
		logear_error("Error: %s no es un comando", false, cmd);
	}

	free(inputline);
}
inline void limpiar_buffer_entrada() {
	int c;
	while ((c = getchar()) != '\n' && c != EOF);
}
void listar_procesos_en_estado(int estado) {

	void imprimir(void *param) {
		Proceso *proceso = param;
		char *status = NULL;
		if (proceso->estado == READY) {
			status = "READY";
		} else if (proceso->estado == EXEC) {
			status = "EXEC";
		} else if (proceso->estado == BLOCKED) {
			status = "BLOCKED";
		} else {
			status = "EXIT";
		}
		if (proceso->estado == estado) {
			logear_info("[%s] Proceso (PID:%d)", status, proceso->pcb->pid);
		}
	}

	if (estado == NEW) {
		int i;
		for (i=0; i < PID_GLOBAL; i++) {
			_Bool distintos(void *param) {
				Proceso *proceso = param;
				return proceso->pcb->pid != i;
			}
			if (list_all_satisfy(procesos, &distintos) && list_all_satisfy(lista_EXIT, &distintos)) {
				logear_info("[NEW] Proceso (PID:%d)", i);
			}
		}
	} else if (estado == EXIT) {
		list_iterate(lista_EXIT, &imprimir);
	} else {
		list_iterate(procesos, &imprimir);
	}
}

//MANEJO DE PROCESOS//
int actualizar_PCB(int socket_cliente, int bytes) {
	void *buffer_PCB = malloc(bytes);
	if (recv(socket_cliente, buffer_PCB, bytes, MSG_WAITALL) <= 0) {
		free(buffer_PCB);
		return -1;
	}
	PCB *pcb = deserializar_PCB(buffer_PCB);
	int PID = pcb->pid;
	free(buffer_PCB);

	Proceso *proceso = proceso_segun_pid(PID);

	if (proceso != NULL) {
		int exit_code = proceso->pcb->exit_code; //Con eso manejo el caso en el que se haya finalizado por consola
		destruir_PCB(proceso->pcb);
		proceso->pcb = pcb;
		if (proceso->estado == EXIT) {
			proceso->pcb->exit_code = exit_code;
		}
	} else {
		destruir_PCB(pcb);
	}

	//Ya que completó el proceso/la ráfaga, ponemos el CPU correspondiente
	//listo para un nuevo proceso
	_Bool mismoCPU(void *param) {
		miCliente *cpu = (miCliente*) param;
		return cpu->socketCliente == socket_cliente;
	}
	miCliente *cpu = list_find(clientes, &mismoCPU);
	cpu->proceso_asociado = NULL;
	//logear_info("CPU con socket %d liberada.", cpu->socketCliente);

	return PID;
}
void agregar_proceso(Proceso *nuevo_proceso) {
	int pid = nuevo_proceso->pcb->pid;
	if (existe_proceso(pid)) {
		logear_info("Warning: Ya existe proceso con mismo PID");
	}
	list_add(procesos, nuevo_proceso);
}
int cantidad_procesos_sistema() {
	return list_size(procesos);
}
void eliminar_proceso(int PID) {
	_Bool mismoPID(void* elemento) {
		return PID == ((Proceso*) elemento)->pcb->pid;
	}
	Proceso* proceso = list_remove_by_condition(procesos,mismoPID);
	_Bool misma_consola(void *param) {
		miCliente *consola = param;
		return consola->socketCliente == proceso->consola;
	}
	miCliente *consola = list_find(clientes, &misma_consola);
	proceso->estado = EXIT;

	if (consola != NULL) {
		if (!consola->va_a_desconectarse) {
			enviar_header(proceso->consola, FINALIZAR_PROGRAMA, sizeof(int));
			send(proceso->consola, &proceso->pcb->pid, sizeof(int), 0);
		}
	}

	limpiar_proceso(proceso);

	list_add(lista_EXIT,proceso);
}
int existe_proceso(int PID) {
	return proceso_segun_pid(PID) == NULL ? false : true;
}
void finalizar_programa(int PID) {
	if (existe_proceso(PID)) {
		Proceso *proceso = proceso_segun_pid(PID);
		if (proceso->cantidad_alocar > proceso->cantidad_liberar) {
			logear_info("[PID:%d] Mem leaks! %d bloques (%d bytes) liberados de %d bloques (%d bytes) reservados a lo largo de %d páginas de Heap",
								PID,
								proceso->cantidad_liberar,
								proceso->bytes_liberados,
								proceso->cantidad_alocar,
								proceso->bytes_alocados,
								proceso->cantidad_paginas_heap);
		}
		if (proceso->cantidad_alocar > 0 && proceso->cantidad_alocar == proceso->cantidad_liberar) {
			logear_info("[PID:%d] Woohoo! El proceso liberó todas las estructuras (%d bloques) a lo largo de sus %d páginas de Heap",
								PID,
								proceso->cantidad_alocar,
								proceso->cantidad_paginas_heap);
		}
		int exit_code = proceso->pcb->exit_code;
		eliminar_proceso(PID);
		mem_finalizar_programa(PID);
		logear_info("[PID:%d] Finalizó con EXIT_CODE:%d", PID, exit_code);
		remover_de_semaforos(PID);
		intentar_iniciar_proceso();
	}
	planificar();
}
void hacer_pedido_memoria(datosMemoria datosMem) {
	int tamanioTotal = sizeof(int) + sizeof(datosMem.codeSize) + datosMem.codeSize;

	enviar_header(socket_memoria, INICIAR_PROGRAMA, tamanioTotal);

	char *buffer = malloc(tamanioTotal);				// Tamanio del codigo

	memcpy(buffer, &datosMem.pid, sizeof(int));		// Como no tengo puntero del pid (de code si), lo paso con &
	memcpy(buffer + sizeof(int), &datosMem.codeSize, sizeof(datosMem.codeSize)); // Aca termino de llenar el buffer que voy a mandar, copie pid primero y dsps codigo
	memcpy(buffer + sizeof(int) + sizeof(datosMem.codeSize), datosMem.code, datosMem.codeSize);

	send(socket_memoria, buffer, tamanioTotal, 0);

	free(buffer);
}
void intentar_desbloquear_proceso(char *nombre_semaforo) {
	Semaforo_QEPD *semaforo = dictionary_get(semaforos, nombre_semaforo);
	Proceso *proceso_bloqueado = list_remove(semaforo->bloqueados, 0);
	_Bool mismo_proceso(void *param) {
		return ((Proceso*)param)->pcb->pid == proceso_bloqueado->pcb->pid;
	}
	Proceso *proceso = list_remove_by_condition(procesos, &mismo_proceso);
	proceso->estado = READY;
	logear_info("Se desbloquea el proceso (PID:%d)", proceso->pcb->pid);
	list_add(procesos, proceso);

	planificar();
}
void intentar_iniciar_proceso() {
	if (cantidad_procesos_sistema() < GRADO_MULTIPROG) {
		Proceso* nuevo_proceso = list_remove(lista_NEW, 0);
		if (nuevo_proceso == NULL) {
			//logear_info("No hay procesos en la cola NEW");
		}
		else {
			char *codigo = strdup(nuevo_proceso->codigo);

			_Bool existe_consola_asociada(void *param) {
				miCliente *consola = param;
				return consola->socketCliente == nuevo_proceso->consola && !consola->va_a_desconectarse;
			}

			if (list_any_satisfy(clientes, &existe_consola_asociada)) {
				int PID = nuevo_proceso->pcb->pid;
				int respuesta = mem_inicializar_programa(PID, strlen(codigo), codigo);

				logear_info("[PID:%d] Pedido a memoria", PID);
				free(codigo);

				if (respuesta < 0) {
					logear_error("[PID:%d] Memoria insuficiente.", false, PID);
					nuevo_proceso->estado = EXIT;
					nuevo_proceso->pcb->exit_code = respuesta;
					limpiar_proceso(nuevo_proceso);
					list_add(lista_EXIT, nuevo_proceso);
					enviar_header(nuevo_proceso->consola, FALLO_INICIO_PROGRAMA, 0);
					intentar_iniciar_proceso();
				}

				else {
					nuevo_proceso->estado = READY;
					agregar_proceso(nuevo_proceso);

					logear_info("Proceso agregado con PID: %d",PID);

					// Le envia el PID a la consola
					enviar_header(nuevo_proceso->consola, INICIAR_PROGRAMA, sizeof(PID));
					send(nuevo_proceso->consola, &PID, sizeof(PID), 0);

					planificar();
				}
			} else {
				logear_error("La consola asociada al proceso (PID:%d) se desconectó, finalizando proceso..", false, nuevo_proceso->pcb->pid);
				nuevo_proceso->estado = EXIT;
				nuevo_proceso->pcb->exit_code = DESCONEXION_CONSOLA;
				limpiar_proceso(nuevo_proceso);
				list_add(lista_EXIT, nuevo_proceso);
				intentar_iniciar_proceso();
			}
		}
	}
}
void limpiar_proceso(Proceso *proceso) {
	void borrar(void *param) {
		Entrada_stack *entrada = (Entrada_stack*) param;
		list_destroy_and_destroy_elements(entrada->args, free);
		list_destroy_and_destroy_elements(entrada->vars, free);
		free(entrada);
	}

	list_destroy_and_destroy_elements(proceso->pcb->indice_stack, &borrar);
	destruir_lista_paginas_heap(proceso);

	destroy_tabla_archivos_proceso(proceso->pcb->tabla_archivos);


	free(proceso->codigo);
	free(proceso->pcb->etiquetas);
	free(proceso->pcb->instrucciones_serializado);

	//Aún tiene memoria reservada para los otros campos.
	//Pero como dicen que hay que mantener la traza de ejecución
	//no los libero, ya que nos interesan saber las estadísticas de los
	//procesos finalizados.
}
void peticion_para_cerrar_proceso(int PID, int exit_code) {
	Proceso *proceso = proceso_segun_pid(PID);

	if (proceso == NULL) {
		logear_info("El proceso (PID:%d) no existe/ya finalizó/no comenzó",PID);
	} else {
		proceso->pcb->exit_code = exit_code;
		if (proceso->estado == EXEC) {
			logear_info("Petición para cerrar el proceso (PID:%d) recibida, espere a la devolución del PCB",PID);
			proceso->estado = EXIT;
		} else {
			logear_info("Petición resuelta ya que (PID:%d) estaba en READY/BLOCKED",PID);
			finalizar_programa(PID);
		}
	}
}
Proceso* proceso_segun_cpu(int socket) {
	_Bool mismo_socket(void *param) {
		miCliente *cpu = param;
		return cpu->socketCliente == socket;
	}
	miCliente *cpu = list_find(clientes, &mismo_socket);
	return cpu->proceso_asociado;
}
Proceso* proceso_segun_pid(int PID) {
	_Bool mismo_pid(void *param) {
		Proceso *proceso = param;
		return proceso->pcb->pid == PID;
	}
	return (Proceso*)list_find(procesos, &mismo_pid);
}
void remover_de_semaforos(int PID) {
	_Bool mismo_pid(void *param) {
		Proceso *proceso = param;
		return proceso->pcb->pid == PID;
	}

	void buscar_pid(char *key, void *data) {
		char *nombre = key;
		Semaforo_QEPD *semaforo = data;
		Proceso *proceso = list_remove_by_condition(semaforo->bloqueados, &mismo_pid);
		if (proceso != NULL) {
			logear_info("Se sacó al proceso (PID:%d) del semáforo %s para que no moleste", PID,  nombre);
		}
	}
	dictionary_iterator(semaforos, &buscar_pid);
}

//MANEJO DE CLIENTES//
void agregar_cliente(char identificador, int socketCliente) {
	if (existe_cliente(socketCliente)) {
		logear_error("No se puede agregar 2 veces mismo socket", false);
		return;
	}

	miCliente *cliente = malloc(sizeof (miCliente));
	cliente->identificador = identificador;
	cliente->socketCliente = socketCliente;
	cliente->proceso_asociado = NULL;
	cliente->va_a_desconectarse = false;

	list_add(clientes, cliente);
}
void borrar_cliente(int socketCliente) {
	DEF_MISMO_SOCKET(socketCliente);
	list_remove_and_destroy_by_condition(clientes, mismoSocket, free);
}
void cerrar_conexion(int socketCliente, char* motivo) {
	logear_error(motivo, false, socketCliente);
	borrar_cliente(socketCliente);
	close(socketCliente);
}
int existe_cliente(int socketCliente) {
	DEF_MISMO_SOCKET(socketCliente);
	return list_any_satisfy(clientes, mismoSocket);
}
int tipo_cliente(int socketCliente) {
	DEF_MISMO_SOCKET(socketCliente);
	miCliente *found = (miCliente*)(list_find(clientes, mismoSocket));
	if (found == NULL) {
		return -1;
	}
	return found->identificador;
}

//PLANIFICACIÓN//
int algoritmo_actual_es(char *algoritmo) {
	return !strcmp(ALGORITMO,algoritmo);
}
miCliente *algun_CPU_disponible() {
	_Bool cpu_lista(void *param) {
		miCliente *cliente = (miCliente*) param;
		return cliente->identificador == CPU && cliente->proceso_asociado == NULL && !cliente->va_a_desconectarse;
	}
	return list_find(clientes, &cpu_lista);
}
Proceso *algun_proceso_listo() {
	_Bool consola_viva(int socket) {
		_Bool misma_consola(void *param) {
			miCliente *consola = param;
			return consola->socketCliente == socket;
		}
		miCliente *consola = list_find(clientes, &misma_consola);
		if (consola != NULL)
			return !consola->va_a_desconectarse;
		return false;
	}

	_Bool proceso_ready(void *param) {
		Proceso *proceso = (Proceso*) param;
		return proceso->estado == READY && consola_viva(proceso->consola);
	}

	return list_remove_by_condition(procesos, &proceso_ready);
}
void inicializar_proceso(int socket, char *codigo, Proceso *nuevo_proceso) {
	nuevo_proceso->consola = socket;
	nuevo_proceso->bytes_alocados = 0;
	nuevo_proceso->bytes_liberados = 0;
	nuevo_proceso->cantidad_alocar = 0;
	nuevo_proceso->cantidad_liberar = 0;
	nuevo_proceso->cantidad_paginas_heap = 0;
	nuevo_proceso->cantidad_rafagas = 0;
	nuevo_proceso->cantidad_syscalls = 0;
	nuevo_proceso->codigo = codigo;
	nuevo_proceso->paginas_heap = list_create();

	t_metadata_program *info = metadata_desde_literal(codigo);
	nuevo_proceso->pcb = malloc(sizeof(PCB));
	nuevo_proceso->pcb->exit_code = 1;
	nuevo_proceso->pcb->program_counter = info->instruccion_inicio;
	nuevo_proceso->pcb->pid = PID_GLOBAL;
	nuevo_proceso->pcb->etiquetas = info->etiquetas;
	nuevo_proceso->pcb->instrucciones_serializado = info->instrucciones_serializado;
	nuevo_proceso->pcb->cantidad_instrucciones = info->instrucciones_size;
	nuevo_proceso->pcb->etiquetas_size = info->etiquetas_size;
	nuevo_proceso->pcb->puntero_stack = 0;
	nuevo_proceso->pcb->cantidad_paginas_codigo = DIVIDE_ROUNDUP(strlen(codigo),tamanio_pagina);
	nuevo_proceso->pcb->indice_stack = list_create();
	nuevo_proceso->pcb->tabla_archivos = list_create();

	//Entrada inicial del stack, no importa inicializar retPos y retVar
	//ya que es el contexto principal y no retorna nada
	Entrada_stack *entrada = malloc(sizeof(Entrada_stack));
	memset(entrada, 0, sizeof(Entrada_stack));
	entrada->args = list_create();
	entrada->vars = list_create();
	list_add(nuevo_proceso->pcb->indice_stack,entrada);

	//File descriptors iniciales
	/*
	int i;
	for (i = 0; i < 3; i++) {
		info_pft *entrada = malloc(sizeof(info_pft));
		memset(entrada, 0, sizeof(info_pft));
		list_add(nuevo_proceso->pcb->tabla_archivos, entrada);
	}
	*/

	free(info);
}
void planificar() {
	miCliente *cpu = algun_CPU_disponible();

	if (planificacion_activa) {
		if (cpu != NULL) {
			Proceso *proceso = algun_proceso_listo();
			if (proceso != NULL) {
				//Se le envia el QUANTUM_SLEEP junto con el PCB
				//ya que este valor es variable a lo largo de la vida del sistema
				if (algoritmo_actual_es("RR")) {
					enviar_header(cpu->socketCliente, QUANTUM_SLEEP, sizeof(int));
					send(cpu->socketCliente, &QUANTUM_SLEEP_VALUE, sizeof(int), MSG_NOSIGNAL);
				}

				proceso->estado = EXEC;
				cpu->proceso_asociado = proceso;
				proceso->cantidad_rafagas++;

				logear_info("Enviando PID %d a ejecución", proceso->pcb->pid);

				int buffersize;
				void *buffer = serializar_PCB(proceso->pcb, &buffersize);

				enviar_header(cpu->socketCliente, PCB_INCOMPLETO, buffersize);
				send(cpu->socketCliente, buffer, buffersize, MSG_NOSIGNAL);
				free(buffer);

				list_add(procesos, proceso);

				planificar(); //Vamos a intentar vaciar la cola de READY
			} else {
				//logear_info("No hay procesos para planificar");
			}
		} else {
			//logear_info("No hay CPUs disponibles");
		}
	} else {
		logear_info("La planificación está desactivada");
	}
}

//SERIALIZADORES y DESERIALIZADORES//
void *list_serialize(t_list* list, int element_size, int *buffersize) {
	int element_count = list_size(list),offset = 8;
	*buffersize = offset + element_count*element_size;
	void *buffer = malloc(*buffersize);
	memcpy(buffer,&element_count,4);
	memcpy(buffer+4,&element_size,4);
	void copy(void *param) {
		memcpy(buffer+offset,param,element_size);
		offset += element_size;
	}
	list_iterate(list,&copy);
	return buffer;
}
t_list *list_deserialize(void *buffer) {
	t_list *new_list = list_create();
	int offset = 8,i,element_count,element_size;
	memcpy(&element_count,buffer,4);
	memcpy(&element_size,buffer+4,4);
	for (i=0;i<element_count;i++) {
		void *element = malloc(element_size);
		memcpy(element,buffer+offset,element_size);
		list_add(new_list,element);
		offset += element_size;
	}
	return new_list;
}
void *serializar_PCB(PCB *pcb, int* buffersize) {
	int instrucciones_size = pcb->cantidad_instrucciones * sizeof(t_intructions);
	int stack_size;
	void *stack_buffer = list_serialize(pcb->indice_stack,sizeof(Entrada_stack), &stack_size);
	void copy(void *param) {
		Entrada_stack *entrada = (Entrada_stack*) param;
		int args_size;
		int vars_size;
		void *args_buffer = list_serialize(entrada->args,sizeof(Posicion_memoria), &args_size);
		void *vars_buffer = list_serialize(entrada->vars,sizeof(Variable), &vars_size);
		stack_buffer = realloc(stack_buffer, stack_size + args_size + vars_size);
		memcpy(stack_buffer + stack_size, args_buffer, args_size);
		memcpy(stack_buffer + stack_size + args_size, vars_buffer, vars_size);
		stack_size += args_size + vars_size;
		free(args_buffer);
		free(vars_buffer);
	}
	list_iterate(pcb->indice_stack,&copy);
	*buffersize = sizeof(PCB) + instrucciones_size + pcb->etiquetas_size + stack_size;
	void *buffer = malloc(*buffersize);
	int offset = 0;
	memcpy(buffer+offset, pcb, sizeof(PCB));
	offset += sizeof(PCB);
	memcpy(buffer + offset, pcb->instrucciones_serializado, instrucciones_size);
	offset += instrucciones_size;
	memcpy(buffer + offset, pcb->etiquetas, pcb->etiquetas_size);
	offset += pcb->etiquetas_size;
	memcpy(buffer + offset, stack_buffer, stack_size);
	free(stack_buffer);
	return buffer;
}
PCB *deserializar_PCB(void *buffer) {
	PCB *pcb = malloc(sizeof(PCB));
	int offset = 0;
	memcpy(pcb, buffer, sizeof(PCB));
	offset += sizeof(PCB);
	int instrucciones_size = pcb->cantidad_instrucciones * sizeof(t_intructions);
	pcb->instrucciones_serializado = malloc(instrucciones_size);
	memcpy(pcb->instrucciones_serializado, buffer + offset, instrucciones_size);
	offset += instrucciones_size;
	pcb->etiquetas = malloc(pcb->etiquetas_size);
	memcpy(pcb->etiquetas, buffer + offset, pcb->etiquetas_size);
	offset += pcb->etiquetas_size;
	pcb->indice_stack = list_deserialize(buffer + offset);
	offset += list_size(pcb->indice_stack) * sizeof(Entrada_stack) + 8;
	void copy(void *param) {
		Entrada_stack *entrada = (Entrada_stack*) param;
		entrada->args = list_deserialize(buffer + offset);
		int args_size = list_size(entrada->args) * sizeof(Posicion_memoria) + 8;
		offset += args_size;
		entrada->vars = list_deserialize(buffer + offset);
		int vars_size = list_size(entrada->vars) * sizeof(Variable) + 8;
		offset += vars_size;
	}
	list_iterate(pcb->indice_stack,&copy);
	return pcb;
}

//EXTRA//
void establecer_configuracion() {
	PUERTO_KERNEL = config_get_int_value(config, "PUERTO_KERNEL");
	logear_info("Puerto Kernel: %d",PUERTO_KERNEL);

	strcpy(IP_MEMORIA,config_get_string_value(config, "IP_MEMORIA"));
	logear_info("IP Memoria: %s",IP_MEMORIA);

	PUERTO_MEMORIA = config_get_int_value(config, "PUERTO_MEMORIA");
	logear_info("Puerto Memoria: %d", PUERTO_MEMORIA);

	strcpy(IP_FS,config_get_string_value(config, "IP_FS"));
	logear_info("IP File System: %s",IP_FS);

	PUERTO_FS = config_get_int_value(config, "PUERTO_FS");
	logear_info("Puerto File System: %d", PUERTO_FS);

	QUANTUM_VALUE = config_get_int_value(config, "QUANTUM");
	logear_info("QUANTUM: %d", QUANTUM_VALUE);

	QUANTUM_SLEEP_VALUE = config_get_int_value(config, "QUANTUM_SLEEP");
	logear_info("QUANTUM_SLEEP: %d", QUANTUM_SLEEP_VALUE);

	strcpy(ALGORITMO,config_get_string_value(config, "ALGORITMO"));
	logear_info("ALGORITMO: %s",ALGORITMO);

	if (algoritmo_actual_es("FIFO"))
		algoritmo_actual = FIFO;
	else if (algoritmo_actual_es("RR"))
		algoritmo_actual = RR;
	else
		logear_error("Algoritmo desconocido, finalizando...", true);


	GRADO_MULTIPROG = config_get_int_value(config, "GRADO_MULTIPROG");
	logear_info("GRADO_MULTIPROG: %d", GRADO_MULTIPROG);

	char **array_semaforos = config_get_array_value(config, "SEM_IDS");
	char **array_semaforos_valores = config_get_array_value(config, "SEM_INIT");

    int i;
    for (i = 0; array_semaforos[i] != NULL; i++) {
    	Semaforo_QEPD *data = malloc(sizeof(Semaforo_QEPD));

    	data->valor = atoi(array_semaforos_valores[i]);
    	data->bloqueados = list_create();

	    dictionary_put(semaforos, array_semaforos[i], data);

	    free(array_semaforos[i]);
	    free(array_semaforos_valores[i]);
    }

    free(array_semaforos);
    free(array_semaforos_valores);

	char **compartidas = config_get_array_value(config, "SHARED_VARS");

	for (i = 0; compartidas[i] != NULL; i++) {

    	int *data = malloc(sizeof(int));
    	*data = 0;

	    dictionary_put(variables_compartidas, compartidas[i]+1, data);

	    free(compartidas[i]);
    }

    free(compartidas);

    void imprimir(char *key, void *param) {
    	Semaforo_QEPD *semaforo = param;
    	logear_info("[%s:%d]", key, semaforo->valor);
    }

    logear_info("Semáforos:");
    dictionary_iterator(semaforos, &imprimir);

    void imprimir2(char *key, void *value) {
    	int *valor = value;
    	logear_info("[!%s:%d]", key, *valor);
    }

    logear_info("Variables compartidas:");
    dictionary_iterator(variables_compartidas, &imprimir2);

	STACK_SIZE = config_get_int_value(config, "STACK_SIZE");
	logear_info("STACK_SIZE: %d", STACK_SIZE);
}
char* remover_salto_linea(char* s) { // By Beej
    int len = strlen(s);

    if (len > 0 && s[len-1] == '\n')  // if there's a newline
        s[len-1] = '\0';          // truncate the string

    return s;
}
int solo_numeros(char *str) {
    while (*str) {
        if (isdigit(*str++) == 0) {
        	return 0;
        }
    }
    return 1;
}
void destruir_PCB(PCB *pcb) {
	void destruir_entrada_stack(void *param) {
		Entrada_stack *entrada = (Entrada_stack *) param;
		list_destroy_and_destroy_elements(entrada->vars, free);
		list_destroy_and_destroy_elements(entrada->args, free);
		free(entrada);
	}
	list_destroy_and_destroy_elements(pcb->indice_stack, &destruir_entrada_stack);
	free(pcb->etiquetas);
	free(pcb->instrucciones_serializado);
	free(pcb);
}
void terminar_kernel() {
	//Sucede cuando se desconecta la memoria
	//(próximamente el file system también)
	logear_info("Finalización de Kernel debido a desconexión de Memoria/FS...");
	log_destroy(logger);
	list_destroy_and_destroy_elements(clientes, free);
	void borrar_proceso(void *param) {
		Proceso *proceso = (Proceso*) param;
		destroy_tabla_archivos_proceso(proceso->pcb->tabla_archivos);
		destruir_PCB(proceso->pcb);
		free(proceso->codigo);
		list_destroy_and_destroy_elements(proceso->paginas_heap, free);
		free(proceso);
	}
	list_destroy_and_destroy_elements(procesos, &borrar_proceso);
	list_destroy_and_destroy_elements(lista_NEW, &borrar_proceso);

	void borrar_proceso_exit(void *param) {
		Proceso *proceso = (Proceso*) param;
		free(proceso->pcb);
		free(proceso);
	}
	list_destroy_and_destroy_elements(lista_EXIT, &borrar_proceso_exit);

	list_destroy_and_destroy_elements(tabla_archivos_global, free);

	void free_semaforo(void *semaforo) {
		t_list *bloqueados = ((Semaforo_QEPD *)semaforo)->bloqueados;
		list_destroy_and_destroy_elements(bloqueados, free);
		free(semaforo);
	}
	dictionary_destroy_and_destroy_elements(semaforos, &free_semaforo);

	dictionary_destroy_and_destroy_elements(variables_compartidas, free);
	printf("Adiós!\n");
	exit(0);
}

int marco_size(void) {
	return tamanio_pagina;
}

int agregar_pagina_heap(int PID) {
	Proceso *proc = proceso_segun_pid(PID);

	if (proc == NULL) {
		return -1;
	}

	/* Nro de pagina agregada = maximo nro de pagina + 1 */
//	int maximo_pagina = proc->pcb->cantidad_paginas_codigo + STACK_SIZE + 1;
//
//	void _maximo_pagina(void *e) {
//		int nro_pagina = ((Pagina_Heap *)e)->nro_pagina;
//
//		if (nro_pagina + 1 > maximo_pagina) {
//			maximo_pagina = nro_pagina + 1;
//		}
//	}
//
//	list_iterate(proc->paginas_heap, _maximo_pagina);

	Pagina_Heap *pagina = malloc(sizeof(Pagina_Heap));

	if (list_is_empty(proc->paginas_heap)) {
		pagina->nro_pagina = proc->pcb->cantidad_paginas_codigo + STACK_SIZE;
	}

	/* Se supone que la ultima pagina agregada tiene el indice maximo */
	else {
		pagina->nro_pagina = ((Pagina_Heap *) list_get(proc->paginas_heap, list_size(proc->paginas_heap) - 1) )->nro_pagina + 1;
	}

	pagina->espacio = tamanio_pagina - sizeof(HeapMetadata);

	list_add(proc->paginas_heap, pagina);
	return pagina->nro_pagina;
}

Pagina_Heap *pagina_heap_proceso(int PID, int nro_pagina) {
	return list_find(lista_paginas_heap_proceso(PID), ({
		bool _buscar_pagina(void *e) {
			return ((Pagina_Heap *)e)->nro_pagina == nro_pagina;
		} _buscar_pagina;
	}));
}

t_list *lista_paginas_heap_proceso(int PID) {
	Proceso *proc = proceso_segun_pid(PID);
	return (proc == NULL) ? NULL : proc->paginas_heap;
}

bool eliminar_pagina_heap(t_list *paginas_heap, int nro_pagina) {
	Pagina_Heap *pagina = list_remove_by_condition(paginas_heap, ({
		bool _buscar_pagina(void *e) {
			return ((Pagina_Heap *)e)->nro_pagina == nro_pagina;
		} _buscar_pagina;
	}));

	if (pagina == NULL) {
		return false;
	}

	free(pagina);
	return true;
}

void destruir_lista_paginas_heap(Proceso *proceso) {
	void _liberar(void *e) {
		liberar_pagina_heap(proceso->paginas_heap, proceso->pcb->pid, ((Pagina_Heap *)e)->nro_pagina);
		free(e);
	}

	list_destroy_and_destroy_elements(proceso->paginas_heap, free);
}
