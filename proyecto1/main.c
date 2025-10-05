#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define BUFFER_SIZE 8192
#define MAX_PROCESSES 32

#define MSG_REQUEST 1
#define MSG_RESULT 2

typedef struct ChunkNode {
	int process_id;
	long file_offset;
	size_t bytes_read;
	double elapsed_time;
	size_t text_length;
	char *text;
	struct ChunkNode *next;
} ChunkNode;

typedef struct {
	char *data;
	size_t length;
	size_t capacity;
} StringBuffer;

typedef struct {
	int process_id;
	long file_offset;
	size_t bytes_read;
	double elapsed_time;
	size_t text_length;
} ResultPayload;

static void fatal(const char *msg) {
	perror(msg);
	exit(EXIT_FAILURE);
}

static ssize_t read_full(int fd, void *buffer, size_t bytes) {
	unsigned char *ptr = buffer;
	size_t total = 0;
	while (total < bytes) {
		ssize_t r = read(fd, ptr + total, bytes - total);
		if (r == 0) {
			return total;
		}
		if (r < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		total += (size_t)r;
	}
	return (ssize_t)total;
}

static ssize_t write_full(int fd, const void *buffer, size_t bytes) {
	const unsigned char *ptr = buffer;
	size_t total = 0;
	while (total < bytes) {
		ssize_t w = write(fd, ptr + total, bytes - total);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		total += (size_t)w;
	}
	return (ssize_t)total;
}

static void string_buffer_init(StringBuffer *buf) {
	buf->data = NULL;
	buf->length = 0;
	buf->capacity = 0;
}

static void string_buffer_reserve(StringBuffer *buf, size_t needed) {
	if (needed <= buf->capacity) return;
	size_t new_cap = buf->capacity ? buf->capacity : 4096;
	while (new_cap < needed) {
		new_cap *= 2;
	}
	char *tmp = realloc(buf->data, new_cap);
	if (!tmp) fatal("realloc");
	buf->data = tmp;
	buf->capacity = new_cap;
}

static void string_buffer_append(StringBuffer *buf, const char *data, size_t len) {
	string_buffer_reserve(buf, buf->length + len + 1);
	memcpy(buf->data + buf->length, data, len);
	buf->length += len;
	buf->data[buf->length] = '\0';
}

static void string_buffer_consume(StringBuffer *buf, size_t len) {
	if (len >= buf->length) {
		buf->length = 0;
		if (buf->data) buf->data[0] = '\0';
		return;
	}
	memmove(buf->data, buf->data + len, buf->length - len);
	buf->length -= len;
	buf->data[buf->length] = '\0';
}

static void string_buffer_free(StringBuffer *buf) {
	free(buf->data);
	buf->data = NULL;
	buf->length = 0;
	buf->capacity = 0;
}

static ChunkNode *create_chunk(const ResultPayload *payload, const char *text) {
	ChunkNode *node = calloc(1, sizeof(ChunkNode));
	if (!node) fatal("calloc");
	node->process_id = payload->process_id;
	node->file_offset = payload->file_offset;
	node->bytes_read = payload->bytes_read;
	node->elapsed_time = payload->elapsed_time;
	node->text_length = payload->text_length;
	if (payload->text_length > 0) {
		node->text = malloc(payload->text_length);
		if (!node->text) fatal("malloc");
		memcpy(node->text, text, payload->text_length);
	}
	return node;
}

static void insert_chunk_sorted(ChunkNode **head, ChunkNode *node) {
	if (!*head || node->file_offset < (*head)->file_offset) {
		node->next = *head;
		*head = node;
		return;
	}
	ChunkNode *curr = *head;
	while (curr->next && curr->next->file_offset < node->file_offset) {
		curr = curr->next;
	}
	node->next = curr->next;
	curr->next = node;
}

static ChunkNode *pop_expected_chunk(ChunkNode **head, long expected_offset) {
	if (!*head || (*head)->file_offset != expected_offset) {
		return NULL;
	}
	ChunkNode *node = *head;
	*head = node->next;
	node->next = NULL;
	return node;
}

static ssize_t find_double_newline(const char *data, size_t len) {
	for (size_t i = 0; i + 1 < len; ++i) {
		if (data[i] == '\n' && data[i + 1] == '\n') {
			return (ssize_t)i;
		}
	}
	return -1;
}

static int process_paragraphs(StringBuffer *carry, regex_t *regex) {
	int chunk_match = 0;
	while (1) {
		ssize_t delim_index = find_double_newline(carry->data, carry->length);
		if (delim_index < 0) break;
		size_t para_len = (size_t)delim_index;
		char original = carry->data[para_len];
		carry->data[para_len] = '\0';
		if (regexec(regex, carry->data, 0, NULL, 0) == 0) {
			printf("%s\n\n", carry->data);
			fflush(stdout);
			chunk_match = 1;
		}
		carry->data[para_len] = original;
		size_t consume = para_len + 2; // elimina párrafo + delimitador
		string_buffer_consume(carry, consume);
	}
	return chunk_match;
}

static int flush_remaining_paragraph(StringBuffer *carry, regex_t *regex) {
	if (carry->length == 0) return 0;
	char saved = carry->data[carry->length];
	carry->data[carry->length] = '\0';
	int match = (regexec(regex, carry->data, 0, NULL, 0) == 0);
	if (match) {
		printf("%s", carry->data);
		if (carry->length == 0 || carry->data[carry->length - 1] != '\n') {
			printf("\n");
		}
		fflush(stdout);
	}
	carry->data[carry->length] = saved;
	carry->length = 0;
	if (carry->data) carry->data[0] = '\0';
	return match;
}

static void write_log_entry(FILE *logfile, int process_id, long offset, size_t bytes_read, double elapsed, int found) {
	fprintf(logfile, "%d,%ld,%zu,%.6f,%d\n", process_id, offset, bytes_read, elapsed, found);
	fflush(logfile);
}

static char *wrap_pattern_with_boundaries(const char *pattern) {
	size_t len = strlen(pattern);
	const char *prefix = "(^|[^[:alnum:]_])(";
	const char *suffix = ")([^[:alnum:]_]|$)";
	size_t total = strlen(prefix) + len + strlen(suffix) + 1;
	char *result = malloc(total);
	if (!result) fatal("malloc");
	sprintf(result, "%s%s%s", prefix, pattern, suffix);
	return result;
}

static void child_process(int id, int pipe_in, int pipe_out, const char *filename) {
	FILE *file = fopen(filename, "r");
	if (!file) fatal("fopen child");
	while (1) {
		int msg_type = MSG_REQUEST;
		if (write_full(pipe_out, &msg_type, sizeof(int)) < 0) fatal("write request type");
		if (write_full(pipe_out, &id, sizeof(int)) < 0) fatal("write request id");
		int end_flag = 0;
		if (read_full(pipe_in, &end_flag, sizeof(int)) <= 0) {
			fatal("read end flag");
		}
		if (end_flag) break;
		long offset = 0;
		size_t bytes = 0;
		if (read_full(pipe_in, &offset, sizeof(long)) <= 0) fatal("read offset");
		if (read_full(pipe_in, &bytes, sizeof(size_t)) <= 0) fatal("read bytes");
		if (fseek(file, offset, SEEK_SET) != 0) fatal("fseek child");
		struct timeval t1, t2;
		gettimeofday(&t1, NULL);
		char buffer[BUFFER_SIZE];
		size_t read_bytes = fread(buffer, 1, bytes, file);
		size_t usable = read_bytes;
		if (read_bytes > 0) {
			size_t last_newline = 0;
			for (size_t j = read_bytes; j > 0; --j) {
				if (buffer[j - 1] == '\n') {
					last_newline = j;
					break;
				}
			}
			if (last_newline > 0) usable = last_newline;
		}
		if (usable == 0 && read_bytes > 0) usable = read_bytes;
		gettimeofday(&t2, NULL);
		double elapsed = (t2.tv_sec - t1.tv_sec) + (t2.tv_usec - t1.tv_usec) / 1e6;
		ResultPayload payload = {id, offset, usable, elapsed, usable};
		msg_type = MSG_RESULT;
		if (write_full(pipe_out, &msg_type, sizeof(int)) < 0) fatal("write result type");
		if (write_full(pipe_out, &payload, sizeof(ResultPayload)) < 0) fatal("write payload");
		if (usable > 0) {
			if (write_full(pipe_out, buffer, usable) < 0) fatal("write buffer");
		}
	}
	fclose(file);
	exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
	if (argc < 5) {
		fprintf(stderr, "Uso: %s <expresion_regular> <archivo> <num_procesos> <logfile>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	char *original_pattern = argv[1];
	char *filename = argv[2];
	int num_procs = atoi(argv[3]);
	if (num_procs < 1 || num_procs > MAX_PROCESSES) {
		fprintf(stderr, "num_procesos debe estar entre 1 y %d\n", MAX_PROCESSES);
		exit(EXIT_FAILURE);
	}
	char *logfilename = argv[4];
	FILE *logfile = fopen(logfilename, "w");
	if (!logfile) fatal("fopen logfile");
	fprintf(logfile, "process_id,file_offset,bytes_read,elapsed_time,found\n");

	char *wrapped_pattern = wrap_pattern_with_boundaries(original_pattern);
	regex_t regex;
	if (regcomp(&regex, wrapped_pattern, REG_EXTENDED | REG_NOSUB) != 0) {
		fprintf(stderr, "Error compilando la expresión regular\n");
		free(wrapped_pattern);
		exit(EXIT_FAILURE);
	}
	free(wrapped_pattern);

	int pipes_in[MAX_PROCESSES][2];
	int pipes_out[MAX_PROCESSES][2];
	pid_t pids[MAX_PROCESSES];

	for (int i = 0; i < num_procs; ++i) {
		if (pipe(pipes_in[i]) < 0) fatal("pipe padre->hijo");
		if (pipe(pipes_out[i]) < 0) fatal("pipe hijo->padre");
	}

	for (int i = 0; i < num_procs; ++i) {
		pid_t pid = fork();
		if (pid < 0) fatal("fork");
		if (pid == 0) {
			for (int j = 0; j < num_procs; ++j) {
				if (j != i) {
					close(pipes_in[j][0]);
					close(pipes_in[j][1]);
					close(pipes_out[j][0]);
					close(pipes_out[j][1]);
				}
			}
			close(pipes_in[i][1]);
			close(pipes_out[i][0]);
			child_process(i, pipes_in[i][0], pipes_out[i][1], filename);
			return 0;
		}
		pids[i] = pid;
	}

	for (int i = 0; i < num_procs; ++i) {
		close(pipes_in[i][0]);
		close(pipes_out[i][1]);
	}

	FILE *file = fopen(filename, "r");
	if (!file) fatal("fopen archivo");
	long next_offset_to_assign = 0;
	long next_offset_to_process = 0;
	ChunkNode *pending_chunks = NULL;
	StringBuffer carry;
	string_buffer_init(&carry);

	int finished_assignments = 0;
	int finished_children = 0;
	int end_sent[MAX_PROCESSES];
	memset(end_sent, 0, sizeof(end_sent));

	while (finished_children < num_procs || pending_chunks) {
		fd_set readfds;
		FD_ZERO(&readfds);
		int maxfd = -1;
		for (int i = 0; i < num_procs; ++i) {
			if (pipes_out[i][0] >= 0) {
				FD_SET(pipes_out[i][0], &readfds);
				if (pipes_out[i][0] > maxfd) maxfd = pipes_out[i][0];
			}
		}
		if (maxfd < 0) break;
		if (select(maxfd + 1, &readfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR) continue;
			fatal("select");
		}
		for (int i = 0; i < num_procs; ++i) {
			int fd = pipes_out[i][0];
			if (fd < 0) continue;
			if (!FD_ISSET(fd, &readfds)) continue;
			int msg_type = 0;
			ssize_t r = read_full(fd, &msg_type, sizeof(int));
			if (r == 0) {
				close(fd);
				pipes_out[i][0] = -1;
				finished_children++;
				continue;
			}
			if (r < 0) fatal("read message type");
			if (msg_type == MSG_REQUEST) {
				int req_id = -1;
				if (read_full(fd, &req_id, sizeof(int)) <= 0) fatal("read request id");
				if (finished_assignments) {
					if (!end_sent[i]) {
						int end_flag = 1;
						write_full(pipes_in[i][1], &end_flag, sizeof(int));
						end_sent[i] = 1;
					}
					continue;
				}
				size_t bytes = 0;
				char tempbuf[BUFFER_SIZE];
				if (fseek(file, next_offset_to_assign, SEEK_SET) != 0) fatal("fseek padre");
				size_t read_bytes = fread(tempbuf, 1, BUFFER_SIZE, file);
				if (read_bytes == 0) {
					finished_assignments = 1;
					if (!end_sent[i]) {
						int end_flag = 1;
						write_full(pipes_in[i][1], &end_flag, sizeof(int));
						end_sent[i] = 1;
					}
					continue;
				}
				size_t last_newline = read_bytes;
				for (size_t j = read_bytes; j > 0; --j) {
					if (tempbuf[j - 1] == '\n') {
						last_newline = j;
						break;
					}
				}
				if (last_newline < read_bytes) bytes = last_newline;
				else bytes = read_bytes;
				if (bytes == 0) bytes = read_bytes;
				long assigned_offset = next_offset_to_assign;
				next_offset_to_assign += (long)bytes;
				int end_flag = 0;
				write_full(pipes_in[i][1], &end_flag, sizeof(int));
				write_full(pipes_in[i][1], &assigned_offset, sizeof(long));
				write_full(pipes_in[i][1], &bytes, sizeof(size_t));
			} else if (msg_type == MSG_RESULT) {
				ResultPayload payload;
				if (read_full(fd, &payload, sizeof(ResultPayload)) <= 0) fatal("read payload");
				char *text = NULL;
				if (payload.text_length > 0) {
					text = malloc(payload.text_length);
					if (!text) fatal("malloc text");
					if (read_full(fd, text, payload.text_length) < (ssize_t)payload.text_length) fatal("read text");
				}
				ChunkNode *node = create_chunk(&payload, text);
				free(text);
				insert_chunk_sorted(&pending_chunks, node);
				while (1) {
					ChunkNode *ready = pop_expected_chunk(&pending_chunks, next_offset_to_process);
					if (!ready) break;
					if (ready->text_length > 0) {
						string_buffer_append(&carry, ready->text, ready->text_length);
					}
					int found = process_paragraphs(&carry, &regex);
					write_log_entry(logfile, ready->process_id, ready->file_offset, ready->bytes_read, ready->elapsed_time, found);
					next_offset_to_process = ready->file_offset + (long)ready->bytes_read;
					free(ready->text);
					free(ready);
				}
			} else {
				fatal("Tipo de mensaje desconocido");
			}
		}
	}

	while (1) {
		ChunkNode *ready = pop_expected_chunk(&pending_chunks, next_offset_to_process);
		if (!ready) break;
		if (ready->text_length > 0) {
			string_buffer_append(&carry, ready->text, ready->text_length);
		}
		int found = process_paragraphs(&carry, &regex);
		write_log_entry(logfile, ready->process_id, ready->file_offset, ready->bytes_read, ready->elapsed_time, found);
		next_offset_to_process = ready->file_offset + (long)ready->bytes_read;
		free(ready->text);
		free(ready);
	}

	flush_remaining_paragraph(&carry, &regex);
	string_buffer_free(&carry);

	for (int i = 0; i < num_procs; ++i) {
		close(pipes_in[i][0]);
		if (pipes_out[i][1] >= 0) close(pipes_out[i][1]);
	}

	for (int i = 0; i < num_procs; ++i) {
		if (pids[i] > 0) waitpid(pids[i], NULL, 0);
	}

	while (pending_chunks) {
		ChunkNode *tmp = pending_chunks;
		pending_chunks = pending_chunks->next;
		free(tmp->text);
		free(tmp);
	}

	fclose(file);
	fclose(logfile);
	regfree(&regex);
	return 0;
}
