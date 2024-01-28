#include "string_replace.h"

#include <errno.h>
#include <stdlib.h>
#include <math.h>
#include <sys/socket.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
//#include <sendfile.h>
#include <unistd.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint8_t winner_id; // top bit used to indicate draw
	uint8_t loser_id;
} match_t;

void ctrlc (int dummy);
int get_player_id(const char * player);
void get_player_by_id(char *res, int id);

static volatile bool keep_running = true;
static int socket_handle;
static int client_fd;

static int n_players = 0;
static char players [1024];
static uint16_t player_ids [127];
static match_t matches [256];
static int n_matches = 0;
static uint16_t elos [127];
static uint16_t wins [127] = {0};
static uint16_t losses [127] = {0};

static uint8_t K_FACTOR = 32;

void add_player_if_not_exist(const char * player) {
	if (strstr(players, player) == NULL) {
		// Player does not exist, so add
		if (strlen(players) + strlen(player) + 1 + 1 > sizeof(players)) {
			printf("Player string has overflowed!");
		}
		strcat(players, "\n");
		// first player gets id=1, id=0 is reserved
		player_ids[++n_players] = strlen(players);
		strcat(players, player);
		elos[n_players] = 1200;
	}
}

char * render(char * file) {
	const size_t count = 2*1024;
	char* template = malloc(count);
	FILE *fp = fopen(file, "r");
	int res = fread(template, 1, count-1, fp);
	template[res] = '\0'; // File read has no terminator!!
	fclose(fp);
	if (res == 0) {
		printf("Template read successful\n");
	} else if (res == -1) {
		printf("Template read failed %d\n", res);
		ctrlc(0);
	} else if (res == count) {
		printf("Probable buffer overflow when reading template!");
		ctrlc(0);
	}

	char with [32];

	sprintf(with, "%d", n_matches);

	char * rendered = str_replace(template, /*replace=*/"%N_MATCHES%", /*with=*/with);

	// Render match table
	rendered = str_replace(
			rendered, 
			"%MATCH_TABLE%", 
			"<table><tr><th>Match</th><th>Winner</th><th>Loser</th><th>Draw</th></tr>\n%MATCH_TABLE%"
			);

	if (n_matches) {
		for (int i=n_matches-1; i >= 0 && i >= n_matches - 10; i--) {
			rendered = str_replace(
					rendered, 
					"%MATCH_TABLE%", 
					"<tr><td>%MID%</td><td>%W%</td><td>%L%</td><td>%D%</td></tr>\n%MATCH_TABLE%"
					);

			sprintf(with, "%d", i);
			rendered = str_replace(rendered, "%MID%", with);

			get_player_by_id(with, matches[i].winner_id & 0x7f);
			rendered = str_replace(rendered, /*replace=*/"%W%", /*with=*/with);

			get_player_by_id(with, matches[i].loser_id & 0x7f);
			rendered = str_replace(rendered, /*replace=*/"%L%", /*with=*/with);

			rendered = str_replace(rendered, /*replace=*/"%D%", /*with=*/matches[i].winner_id & (1u<<7) ? "yes" : "no");
		}
	}

	rendered = str_replace(rendered, "%MATCH_TABLE%", "</table>\n");

	// Render ratings table
	rendered = str_replace(
			rendered, 
			"%RATINGS_TABLE%", 
			"<table><tr><th>Player</th><th>ELO</th><th>Wins</th><th>Losses</th></tr>\n%RATINGS_TABLE%"
			);

	// sometimes finds 2x RATINGS_TABLE!

	for (int i=1; i<=n_players; i++) {
		rendered = str_replace(
				rendered, 
				"%RATINGS_TABLE%", 
				"<tr><td>%P%</td><td>%E%</td><td>%W%</td><td>%L%</td></tr>\n%RATINGS_TABLE%"
				);

		get_player_by_id(with, i);
		rendered = str_replace(rendered, /*replace=*/"%P%", /*with=*/with);

		sprintf(with, "%d", elos[i]);
		rendered = str_replace(rendered, "%E%", with);

		sprintf(with, "%d", wins[i]);
		rendered = str_replace(rendered, "%W%", with);

		sprintf(with, "%d", losses[i]);
		rendered = str_replace(rendered, "%L%", with);
	}
	rendered = str_replace(rendered, "%RATINGS_TABLE%", "</table>\n");

	return rendered;
}

void print_match(const match_t * match) {
	bool draw = match->winner_id & (1u << 7);
	printf("Match:\n\tWinner: %d\n\tLoser: %d\n\tDraw: %b\n",
			match->winner_id & 0x7f, match->loser_id, draw);
}

int get_player_id(const char * player) {
	for (int i=0; i<sizeof(player_ids)/2; i++) {
		if (0==strncmp(&players[player_ids[i]], player, strlen(player))) {
			//printf("Found player, id is %d\n", i);
			return i;
		}
	}
}

void get_player_by_id(char *res, int id) {
	if ( id == 0 ) {
		printf("get_player_by_id(id=0) is not allowed!\n");
		res = 0;
	} else if (id == n_players) {
		// Last player is not terminated by \n
		sprintf(res, "%s", &players[player_ids[id]]);
	} else {
		int len = strstr(&players[player_ids[id]], "\n") - &players[player_ids[id]];
		memcpy(res, &players[player_ids[id]], len);
		res[len] = 0;
	}
}

void game_logic(const char * key, const char * value, bool done) {
	static match_t current_match = {};

	if (!strcmp(key,"winner")) {
		add_player_if_not_exist(value);
		current_match.winner_id = (current_match.winner_id & (1u<<7)) | get_player_id(value);
	}

	if (!strcmp(key,"loser")) {
		add_player_if_not_exist(value);
		current_match.loser_id = get_player_id(value);
	}

	if (!strcmp(key,"draw") && !strcmp(value, "on")) {
		current_match.winner_id |= (1u<<7);
	}

	if (done) {
		// Match has completed, store it and update ELOs
		//print_match(&current_match);
		memcpy(&matches[n_matches++], &current_match, sizeof(match_t));

		// Update ELO
		// E_a = 1 / (1 + 10^(R_b-R_a)/400
		// E_b = 1 / (1 + 10^(R_a-R_b)/400
		// R_a' = R_a + K(S_a - E_a)
		// R_b' = R_b + K(S_b - E_b)
		const uint16_t winner_elo = elos[current_match.winner_id];
		const uint16_t loser_elo = elos[current_match.loser_id];

		const float winner_score = current_match.winner_id & (1u<<7) ? 0.5f : 1.0f;
		const float loser_score = current_match.winner_id & (1u<<7) ? 0.5f : 0.0f;

		const float winner_expected = 1 / (1 + powf(10.0, (winner_elo - loser_elo) / 400.0));
		const float loser_expected = 1 / (1 + powf(10.0, (loser_elo - winner_elo) / 400.0));

		const uint8_t winner_id = current_match.winner_id & 0x7f;
		const uint8_t loser_id = current_match.loser_id & 0x7f;

		printf("Winner expected %.3f\n", winner_expected);
		printf("Loser expected %.3f\n", loser_expected);

		elos[winner_id] += (uint16_t)round(K_FACTOR*(winner_score - winner_expected));
		elos[loser_id] += (uint16_t)round(K_FACTOR*(loser_score - loser_expected));

		wins[winner_id]++;
		losses[loser_id]++;

		printf("Winner change %.3f\n", K_FACTOR*(winner_score - winner_expected));
		printf("Loser change %.3f\n", K_FACTOR*(loser_score - loser_expected));
	
		// Prepare next match
		current_match.winner_id = 0;
		current_match.loser_id = 0;
	}
}

void post (const char * request) {
	printf("Received POST request\n");
	//printf("%s\n", request);
	const char * delimiter = "\r\n\r\n";
	char * body = strstr(request, delimiter) + strlen(delimiter);
	if (body) {
		// Based on example from man strtok
		// this exotic for loop ensures strtok
		// gets body the first time it calls, but 
		// thereafter gets NULL as the first argument
		char *token, *subtoken;
		char *saveptr1, *saveptr2;
		for (char *str1 = body; ; str1 = NULL) {
			token = strtok_r(str1, "&", &saveptr1);
			if (token == NULL) {
				break;
			}
			// From each key-value pair, get key and value
			subtoken = strtok_r(token, "=", &saveptr2);
			if (subtoken == NULL) {
				break;
			}
			char * key = subtoken;

			subtoken = strtok_r(NULL, "=", &saveptr2);
			if (subtoken == NULL) {
				printf("No value given for %s -> discarding incomplete entry\n");
				return;
			}
			char * value = subtoken;
			game_logic(key, value, false);
		}
		// Save the game
		game_logic("" ,"", true);
	} else {
		printf("Found no body\n");

	}
}

void ctrlc (int dummy) {
	printf("Program interrupted!");
	keep_running = false;
	//close(client_fd);
	close(socket_handle);
	sleep(1);
	exit(0);
}

void main () {
	signal(SIGINT, ctrlc);

	int socket_handle = socket(AF_INET, SOCK_STREAM, 0);

	struct sockaddr_in addr = {
		AF_INET,
		0x901f, // port 8080
		0, // accept all source addrs
	};

	int res = bind(socket_handle, &addr, sizeof(addr));
	if (res) {
		printf("bind failed with error code %d", res);
		exit(1);
	}

	listen(socket_handle,10);

	while(keep_running) {
		client_fd = accept(socket_handle, 0, 0);

		char buffer[4096] = {0};
		recv(client_fd, buffer, 4096, 0);

		for (int i = 0; i < sizeof(buffer); ++i) {
			putc(buffer[i], stdout);
		}
		putc('\n', stdout);


		if (strncmp("GET", buffer, 3) == 0) { 
			printf("Received GET request\n");
			//printf("%s\n", buffer);
		} else if (strncmp("POST", buffer, 4) == 0) { 
			post(buffer);
			// TODO: redirect page to avoid re-posting
			// see https://stackoverflow.com/a/29191719
		}

		// GET /file.html ....
		// skip 5 chars for GET
		//char* f = buffer + 5;

		// terminate string at first space after file ' '
		//*strchr(f, ' ') = 0;
		//int opened_fd = open(f, O_RDONLY);
		//int opened_fd = open("index.html", O_RDONLY);
		char * rendered = render("index.html");
		//close(opened_fd);
		if (!rendered) {
			printf("Render failed\n");
			ctrlc(0);
		}
		//printf("%s\n", rendered);
		int res = send(client_fd, rendered, strlen(rendered), 0);
		//int res = send(client_fd, rendered, sizeof(rendered));
		printf("Wrote %d bytes to client socket\n", res);
		if (res == -1) {
			int errsv = errno;
			printf("Write to socket failed with: %d\n", errsv);
		} 
		free(rendered);
		//sendfile(client_fd, opened_fd, 0, sizeof(buffer));
		close(client_fd);
	}
}
