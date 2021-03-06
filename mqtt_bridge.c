#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <mosquitto.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <termios.h>
#include "mqtt_bridge.h"

#define MAX_NODE 254
#define DEF_PORT "/dev/ttyUSB0"
#define DEF_RATE B57600

char line_buf[2048];
char *line_buf_p;

void assemble_line(char *, int);
void process(char *);
void send(int node, int sensor, int type, int sub_type, char *payload);
void debug(const char *, ...);
int find_free_node();
void record_publish(char *topic);
int throttled(char *topic);

int port;
int nodes[MAX_NODE];
struct mosquitto *mosq;
int debug_flag = 0;
int throttle = 0;
time_t last_publish = 0;

struct publish_entry {
	time_t			last_publish;
	char *			topic;
	struct publish_entry	*next;
};

struct publish_entry *publish_record;

main(int argc, char *argv[])
{
	char buf[2048];
	int i;
	size_t len;
	int j;
	char *port_name = NULL;
	int foreground_flag = 0;
	int opt;
	struct termios oldtio, newtio;

	while ((opt = getopt(argc, argv, "fdp:t:")) != -1) {
		switch (opt) {
		case 'f':
			foreground_flag = 1;
			break;
		case 'd':
			debug_flag = 1;
			/* Debug implies foreground */
			foreground_flag = 1;
			break;
		case 'p':
			port_name = optarg;
			break;
		case 't':
			throttle = atoi(optarg);
			break;
		default:
			fprintf(stderr, "Usage: %s [-f] [-d] [-t <throttle>] [-p <port>]\n", argv[0]);
			fprintf(stderr, "  -f: run in the foreground.  Default behavior is to run as a daemon.\n");
			fprintf(stderr, "  -d: enable debugging.  Implies -f\n");
			fprintf(stderr, "  -t: minimum number of seconds between MQTT publishes\n");
			fprintf(stderr, "  -p: specify serial port.  Devault is %s\n", DEF_PORT);
			exit(EXIT_FAILURE);
			break;
		}
	}

	publish_record = NULL;

	debug("Starting\n");

	if (port_name == NULL) {
		port_name = DEF_PORT;
	}
	port = open(port_name, O_RDWR);

	if (port < 0) {
		perror("open");
		fprintf(stderr, "Can't open port %s\n", port_name); 
		exit(EXIT_FAILURE);
	}

	debug("Port open\n");

	if (!foreground_flag) {
		daemon(0, 0);
	}

	tcgetattr(port, &oldtio);
	bzero(&newtio, sizeof(newtio));

	newtio.c_cflag = DEF_RATE | CLOCAL | CREAD;

	tcflush(port, TCIFLUSH);
	tcsetattr(port, TCSANOW, &newtio);

	line_buf[0] = '\0';
	line_buf_p = &(line_buf[0]);

	memset(nodes, 0, sizeof(nodes));

	mosq = mosquitto_new("rf69_bridge", true, NULL);
        if (mosq == NULL) {
                perror("mosquitto_new");
                mosquitto_lib_cleanup();
                exit(EXIT_FAILURE);
        }

        if (mosquitto_connect(mosq, "localhost", 1883, 10) != MOSQ_ERR_SUCCESS) {
                fprintf(stderr, "mosquitto_connect failed\n");
                mosquitto_lib_cleanup();
                exit(EXIT_FAILURE);
        }

        mosquitto_loop_start(mosq);


	while (1) {
		fd_set readfds;

		FD_ZERO(&readfds);
		FD_SET(port, &readfds);
		select(port+1, &readfds, NULL, NULL, NULL);
		len = read(port, buf, sizeof(buf));
		if (len == 0) {
			continue;
		}
		buf[len] = '\0';
		assemble_line(buf, len);
	}
}

void assemble_line(char *buf, int len)
{
	char *newline;
	char *unprocessed;

	unprocessed = buf;

	while (*unprocessed != 0) {
		char *last;
		newline = strchr(unprocessed, '\n');

		if (newline == NULL) {
			// No newline in the buf, just append it
			strcat(line_buf, unprocessed);
			return;
		}

		*newline = '\0';
		/* Check for \r */
		last = &(unprocessed[strlen(unprocessed)-1]);
		if (*last == '\r') {
			*last = '\0';
		}
		strcat(line_buf, unprocessed);
		process(line_buf);
		line_buf[0] = '\0';

		unprocessed = newline;
		++unprocessed;
	}
}

void process(char *line)
{
	char *token;
	int node;
	int sensor;
	int type;
	int sub_type;
	char *payload;

	debug("Line: %s\n", line);

	token = strtok(line, ";");
	if (token == NULL) {
		return;
	}
	node = strtol(token, NULL, 10);
	token = strtok(NULL, ";");
	if (token == NULL) {
		return;
	}
	sensor = strtol(token, NULL, 10);
	token = strtok(NULL, ";");
	if (token == NULL) {
		return;
	}
	type = strtol(token, NULL, 10);
	token = strtok(NULL, ";");
	if (token == NULL) {
		return;
	}
	sub_type = strtol(token, NULL, 10);
	payload = strtok(NULL, ";");
	if (token == NULL) {
		return;
	}

	if (node <= MAX_NODE) {
		if (nodes[node] == 0) {
			nodes[node] = 1;
			debug("0:0: Reserved node %d\n", node);
		}
	}

	switch(type) {
	case C_PRESENTATION: // Presentation
		switch(sub_type) {
		case S_ARDUINO_NODE:
			debug ("%d:%d: New Arduino node, version %s\n", node, sensor, payload);
			break;
		default:	
			debug("%d:%d: SubType %s, Presentation: '%s'\n",
				node, sensor, presentation_subtype_names[sub_type], 
				payload);
			break;
		}
		break;
	case C_SET: // Set
		{
			char topic[80];
			int rc;

			debug("%d:%d: SubType %s, Set: '%s'\n",
				node, sensor, data_subtype_names[sub_type], payload);
		
			sprintf(topic, "mqtt/%d/%d/%s", node, sensor, 
				data_subtype_names[sub_type]);
			if (!throttled(topic)) {
				debug("Publishing topic '%s' message '%s'\n", topic, payload);
				rc = mosquitto_publish(mosq, NULL, topic, strlen(payload), payload, 1, true);
                		if (rc  != MOSQ_ERR_SUCCESS) {
                        		perror("publish");
                        		fprintf(stderr, "publish failed, returned %d\n", rc);
                        		mosquitto_reconnect(mosq);
                		}
				record_publish(topic);
			} else {
				debug("Topic %s is throttled, holding\n", topic);
			}
		}
		break;
	case C_INTERNAL: // Internal
		switch(sub_type) {
		case I_ID_REQUEST: {
			int next_node;
			char node_str[5];

			next_node = find_free_node();
			snprintf(node_str, 5, "%d", next_node);
			send(node, sensor, C_INTERNAL, I_ID_RESPONSE, node_str);
			debug("%d:%d: Got ID request, assigned node %d\n", node, sensor, next_node);
			}
			break;
		case I_CONFIG:
			send(node, sensor, C_INTERNAL, I_CONFIG, "I");
			debug("%d:%d: Got CONFIG request, sent reply\n", node, sensor);
			break;
		case I_SKETCH_NAME:
			debug("%d:%d: Sketch Name: %s\n", node, sensor, payload);
			break;
		case I_SKETCH_VERSION:
			debug("%d:%d: Sketch Version: %s\n", node, sensor, payload);
			break;
		default:
			debug("%d:%d: Type INTERNAL, SubType %s, payload '%s'\n",
				node, sensor,  
				internal_subtype_names[sub_type], payload);
			break;
		}
		break;
	default:
		debug("%d:%d: Type %s, SubType %d, Payload '%s'\n",
			node, sensor, message_type_names[type],  
			sub_type, payload);
		break;
	}
}

void send(int node, int sensor, int type, int sub_type, char *payload)
{
	char line[2048]; 
	snprintf(line, sizeof(line), "%d;%d;%d;%d;%s\n", 
		node, sensor, type, sub_type, payload);

	write(port, line, strlen(line));
}

int find_free_node()
{
	int i;

	for (i=0; i<MAX_NODE; i++) {
		if (nodes[i] == 0) {
			nodes[i] = 1;
			return i;
		}
	}

	return MAX_NODE+1;
}

void debug(const char *fmt, ...)
{
	va_list ap;

	if (debug_flag) {
		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
	}
}

/**
 * Return zero if it's OK to publish a topic, non-zero if the
 * throttle time has not been reached.
 */
int throttled(char *topic)
{
	time_t now;
	struct publish_entry *entry;

	entry = publish_record;
	
	while (entry != NULL) {
		if (strcmp(entry->topic, topic) == 0) {
			break;
		}
		entry = entry->next;
	}

	if (entry == NULL) {
		/* We've not published this topic before */
		return 0;
	}

	now = time(NULL);

	debug("topic: %s; now: %ld;  last_publish: %ld;  throttle: %d\n", topic, now, entry->last_publish, throttle);

	if (now > (entry->last_publish + throttle)) {
		return 0;
	} else {
		return 1;
	}
}

void record_publish(char *topic)
{
	struct publish_entry *entry;

	entry = publish_record;
	
	while (entry != NULL) {
		if (strcmp(entry->topic, topic) == 0) {
			break;
		}
		entry = entry->next;
	}

	if (entry == NULL) {
		/* We've not published this topic before */
		entry = malloc(sizeof(*entry));
		entry->topic = malloc(strlen(topic) + 1);
		strcpy(entry->topic, topic);
		entry->last_publish = time(NULL);
		entry->next = publish_record;
		publish_record = entry;
	} else {
		entry->last_publish = time(NULL);
	}
}
