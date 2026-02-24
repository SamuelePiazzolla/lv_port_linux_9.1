/*
 * wpa_supplicant/hostapd control interface library
 * Standalone Version - Linux Embedded / LVGL Integration
 *
 * Derivato da: wpa_supplicant/src/common/wpa_ctrl.c
 * Copyright (c) 2004-2007, Jouni Malinen <j@w1.fi>
 * BSD License
 *
 * Modifiche rispetto all'originale:
 *  - Rimosso include "includes.h" e "common.h" (runtime wpa_supplicant interno)
 *  - Sostituiti os_* con equivalenti stdlib standard (malloc, free, snprintf...)
 *  - Rimossi branch UDP, Android, Windows Named Pipe (non necessari)
 *  - Mantenuta gestione EAGAIN su send() per socket O_NONBLOCK
 *  - Aggiunto check IFNAME= in wpa_ctrl_request per compatibilità global ctrl
 *  - Corretta gestione errore in wpa_ctrl_pending()
 *  - Socket impostato O_NONBLOCK: fondamentale per integrazione loop LVGL
 *    (usare wpa_ctrl_get_fd() + poll/select nel tick LVGL per eventi asincroni)
 *
 */

#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "wpa_ctrl.h"

/* =========================================================================
 * Configurazione percorsi socket client
 * ========================================================================= */

#ifndef CONFIG_CTRL_IFACE_CLIENT_DIR
#define CONFIG_CTRL_IFACE_CLIENT_DIR "/tmp"
#endif

#ifndef CONFIG_CTRL_IFACE_CLIENT_PREFIX
#define CONFIG_CTRL_IFACE_CLIENT_PREFIX "wpa_ctrl_"
#endif

/* Timeout in secondi per wpa_ctrl_request().
 * 10s è il valore originale wpa_supplicant: necessario per comandi lenti
 * come SCAN o CONNECT. NON usare wpa_ctrl_request() bloccante nel tick LVGL;
 * usare invece wpa_ctrl_pending() + wpa_ctrl_recv() sulla connessione monitor. */
#define WPA_CTRL_REQUEST_TIMEOUT_SEC 10

/* =========================================================================
 * Helper interno: strlcpy portabile
 *
 * Sostituisce os_strlcpy() dell'originale.
 * Garantisce sempre NUL-termination, ritorna strlen(src) per rilevare
 * troncamento (ret >= size => troncato).
 * ========================================================================= */
static size_t wpa_ctrl_strlcpy(char *dest, const char *src, size_t size)
{
	const char *s = src;
	size_t left = size;

	if (left) {
		while (--left != 0) {
			if ((*dest++ = *s++) == '\0')
				break;
		}
	}

	/* Garantisci NUL-termination e calcola lunghezza totale src */
	if (left == 0) {
		if (size != 0)
			*dest = '\0';
		while (*s++)
			;
	}

	return (size_t)(s - src - 1);
}

/* =========================================================================
 * Helper interno: timestamp relativo in secondi (per retry EAGAIN)
 *
 * Sostituisce os_get_reltime() + os_reltime_expired() dell'originale.
 * ========================================================================= */
static double wpa_ctrl_now_sec(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0.0;
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* =========================================================================
 * Struttura di controllo interna
 *
 * Opaca per il chiamante (vedi wpa_ctrl.h): accesso solo tramite API.
 * ========================================================================= */
struct wpa_ctrl {
	int s;                      /* File descriptor socket UNIX DGRAM */
	struct sockaddr_un local;   /* Socket client (in /tmp) */
	struct sockaddr_un dest;    /* Socket server wpa_supplicant */
};

/* =========================================================================
 * wpa_ctrl_open / wpa_ctrl_open2
 * ========================================================================= */

struct wpa_ctrl *wpa_ctrl_open(const char *ctrl_path)
{
	return wpa_ctrl_open2(ctrl_path, NULL);
}

struct wpa_ctrl *wpa_ctrl_open2(const char *ctrl_path, const char *cli_path)
{
	struct wpa_ctrl *ctrl;
	static int counter = 0;
	int ret;
	size_t res;
	int tries = 0;
	int flags;

	if (ctrl_path == NULL)
		return NULL;

	ctrl = calloc(1, sizeof(*ctrl));
	if (ctrl == NULL)
		return NULL;

	ctrl->s = socket(PF_UNIX, SOCK_DGRAM, 0);
	if (ctrl->s < 0) {
		free(ctrl);
		return NULL;
	}

	ctrl->local.sun_family = AF_UNIX;
	counter++;

try_again:
	/* Costruisce il path del socket client locale in /tmp (o cli_path) */
	if (cli_path && cli_path[0] == '/') {
		ret = snprintf(ctrl->local.sun_path,
			       sizeof(ctrl->local.sun_path),
			       "%s/" CONFIG_CTRL_IFACE_CLIENT_PREFIX "%d-%d",
			       cli_path, (int)getpid(), counter);
	} else {
		ret = snprintf(ctrl->local.sun_path,
			       sizeof(ctrl->local.sun_path),
			       CONFIG_CTRL_IFACE_CLIENT_DIR "/"
			       CONFIG_CTRL_IFACE_CLIENT_PREFIX "%d-%d",
			       (int)getpid(), counter);
	}

	/* snprintf: ret < 0 => errore formattazione, ret >= size => troncato */
	if (ret < 0 || (size_t)ret >= sizeof(ctrl->local.sun_path)) {
		close(ctrl->s);
		free(ctrl);
		return NULL;
	}

	tries++;
	if (bind(ctrl->s, (struct sockaddr *)&ctrl->local,
		 sizeof(ctrl->local)) < 0) {
		if (errno == EADDRINUSE && tries < 2) {
			/* Socket rimasto da una corsa precedente: rimuovi e riprova */
			unlink(ctrl->local.sun_path);
			goto try_again;
		}
		close(ctrl->s);
		free(ctrl);
		return NULL;
	}

	/* Configura indirizzo destinazione (wpa_supplicant) */
	ctrl->dest.sun_family = AF_UNIX;

	if (strncmp(ctrl_path, "@abstract:", 10) == 0) {
		/* Abstract UNIX socket: primo byte '\0', nessun file su filesystem */
		ctrl->dest.sun_path[0] = '\0';
		wpa_ctrl_strlcpy(ctrl->dest.sun_path + 1, ctrl_path + 10,
				 sizeof(ctrl->dest.sun_path) - 1);
	} else {
		res = wpa_ctrl_strlcpy(ctrl->dest.sun_path, ctrl_path,
				       sizeof(ctrl->dest.sun_path));
		if (res >= sizeof(ctrl->dest.sun_path)) {
			close(ctrl->s);
			unlink(ctrl->local.sun_path);
			free(ctrl);
			return NULL;
		}
	}

	if (connect(ctrl->s, (struct sockaddr *)&ctrl->dest,
		    sizeof(ctrl->dest)) < 0) {
		close(ctrl->s);
		unlink(ctrl->local.sun_path);
		free(ctrl);
		return NULL;
	}

	/*
	 * Imposta socket non-bloccante.
	 * FONDAMENTALE per l'integrazione con il loop LVGL:
	 * evita che send()/recv() blocchino il rendering in caso di
	 * wpa_supplicant lento o irraggiungibile.
	 */
	flags = fcntl(ctrl->s, F_GETFL);
	if (flags >= 0) {
		flags |= O_NONBLOCK;
		if (fcntl(ctrl->s, F_SETFL, flags) < 0) {
			/* Non fatale: il socket resterà bloccante */
			perror("wpa_ctrl: fcntl O_NONBLOCK");
		}
	}

	return ctrl;
}

/* =========================================================================
 * wpa_ctrl_close
 * ========================================================================= */

void wpa_ctrl_close(struct wpa_ctrl *ctrl)
{
	if (ctrl == NULL)
		return;
	unlink(ctrl->local.sun_path); /* Rimuove file socket client da /tmp */
	if (ctrl->s >= 0)
		close(ctrl->s);
	free(ctrl);
}

/* =========================================================================
 * wpa_ctrl_request
 *
 * Invia un comando a wpa_supplicant e attende la risposta.
 * Gestisce:
 *  - EAGAIN/EWOULDBLOCK su send() (socket O_NONBLOCK): retry fino a 5s
 *  - Messaggi asincroni in arrivo durante l'attesa (prefisso '<' o 'IFNAME=')
 *  - Timeout reale (WPA_CTRL_REQUEST_TIMEOUT_SEC secondi)
 *
 * ATTENZIONE: questa funzione è BLOCCANTE fino a timeout.
 * Per il loop LVGL usare ESCLUSIVAMENTE sulla connessione comandi (cmd_ctrl),
 * mai sulla connessione monitor (mon_ctrl). Per gli eventi usare
 * wpa_ctrl_pending() + wpa_ctrl_recv() in un handler non bloccante.
 * ========================================================================= */

int wpa_ctrl_request(struct wpa_ctrl *ctrl, const char *cmd, size_t cmd_len,
		     char *reply, size_t *reply_len,
		     void (*msg_cb)(char *msg, size_t len))
{
	struct timeval tv;
	int res;
	fd_set rfds;
	double started_at = 0.0;

	/*
	 * send() con socket O_NONBLOCK: può ritornare EAGAIN se il buffer
	 * del kernel è pieno. Riproviamo per un massimo di 5 secondi,
	 * dormendo 1ms tra un tentativo e l'altro per non bruciare CPU.
	 */
retry_send:
	if (send(ctrl->s, cmd, cmd_len, 0) < 0) {
		if (errno == EAGAIN || errno == EBUSY || errno == EWOULDBLOCK) {
			if (started_at == 0.0)
				started_at = wpa_ctrl_now_sec();
			else if (wpa_ctrl_now_sec() - started_at > 5.0)
				return -1; /* Timeout send */

			/* Attesa minima per non saturare la CPU */
			struct timespec ns = { 0, 1000000 }; /* 1 ms */
			nanosleep(&ns, NULL);
			goto retry_send;
		}
		return -1;
	}

	/* Loop di ricezione: scarta messaggi asincroni, attende la risposta */
	for (;;) {
		tv.tv_sec  = WPA_CTRL_REQUEST_TIMEOUT_SEC;
		tv.tv_usec = 0;
		FD_ZERO(&rfds);
		FD_SET(ctrl->s, &rfds);

		res = select(ctrl->s + 1, &rfds, NULL, NULL, &tv);

		if (res < 0 && errno == EINTR)
			continue; /* Segnale ricevuto: riprova select */
		if (res < 0)
			return -1;
		if (res == 0)
			return -2; /* Timeout reale: nessuna risposta */

		if (!FD_ISSET(ctrl->s, &rfds))
			return -2;

		res = recv(ctrl->s, reply, *reply_len, 0);
		if (res < 0)
			return -1;

		/*
		 * Filtra messaggi asincroni non sollecitati che possono arrivare
		 * sulla connessione comandi se wpa_ctrl_attach() è stato chiamato:
		 *
		 *  '<N>...'   : evento wpa_supplicant (es. <3>CTRL-EVENT-SCAN-RESULTS)
		 *  'IFNAME=..' : risposta da global ctrl interface (/var/run/wpa_supplicant/global)
		 *
		 * Vengono passati al callback msg_cb se registrato, poi si
		 * continua ad attendere la risposta al comando.
		 */
		if ((res > 0 && reply[0] == '<') ||
		    (res > 6 && strncmp(reply, "IFNAME=", 7) == 0)) {
			if (msg_cb) {
				size_t tmp_len = (size_t)res;
				if (tmp_len >= *reply_len)
					tmp_len = *reply_len - 1;
				reply[tmp_len] = '\0';
				msg_cb(reply, tmp_len);
			}
			continue; /* Aspetta la vera risposta al comando */
		}

		*reply_len = (size_t)res;
		break;
	}

	return 0;
}

/* =========================================================================
 * wpa_ctrl_attach / wpa_ctrl_detach
 *
 * Registra/deregistra la connessione come monitor eventi.
 * Chiamare SOLO sulla connessione dedicata agli eventi (mon_ctrl),
 * mai sulla connessione comandi (cmd_ctrl).
 * ========================================================================= */

static int wpa_ctrl_attach_helper(struct wpa_ctrl *ctrl, int attach)
{
	char buf[10];
	int ret;
	size_t len = sizeof(buf);

	ret = wpa_ctrl_request(ctrl,
			       attach ? "ATTACH" : "DETACH", 6,
			       buf, &len, NULL);
	if (ret < 0)
		return ret;

	if (len == 3 && memcmp(buf, "OK\n", 3) == 0)
		return 0;

	return -1;
}

int wpa_ctrl_attach(struct wpa_ctrl *ctrl)
{
	return wpa_ctrl_attach_helper(ctrl, 1);
}

int wpa_ctrl_detach(struct wpa_ctrl *ctrl)
{
	return wpa_ctrl_attach_helper(ctrl, 0);
}

/* =========================================================================
 * wpa_ctrl_recv
 *
 * Legge un messaggio evento dalla connessione monitor.
 * Il socket è O_NONBLOCK: se non ci sono dati ritorna EAGAIN (errno).
 * Chiamare solo dopo aver verificato con wpa_ctrl_pending() o dopo
 * che il proprio poll/select ha segnalato il fd come leggibile.
 * ========================================================================= */

int wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t *reply_len)
{
	int res;

	res = recv(ctrl->s, reply, *reply_len, 0);
	if (res < 0)
		return -1;

	*reply_len = (size_t)res;
	return 0;
}

/* =========================================================================
 * wpa_ctrl_pending
 *
 * Controlla se ci sono messaggi evento in attesa (non bloccante).
 * Ritorna: 1 se ci sono dati, 0 se nessun dato, -1 se errore select.
 *
 * Uso tipico nel loop LVGL:
 *   if (wpa_ctrl_pending(mon_ctrl) > 0) {
 *       size_t len = sizeof(buf);
 *       wpa_ctrl_recv(mon_ctrl, buf, &len);
 *       // gestisci evento
 *   }
 * ========================================================================= */

int wpa_ctrl_pending(struct wpa_ctrl *ctrl)
{
	struct timeval tv = { 0, 0 }; /* Non bloccante: timeout zero */
	fd_set rfds;
	int res;

	FD_ZERO(&rfds);
	FD_SET(ctrl->s, &rfds);

	res = select(ctrl->s + 1, &rfds, NULL, NULL, &tv);
	if (res < 0)
		return -1; /* Errore: propagato al chiamante */

	return FD_ISSET(ctrl->s, &rfds) ? 1 : 0;
}

/* =========================================================================
 * wpa_ctrl_get_fd
 *
 * Ritorna il file descriptor del socket.
 * Usare nel loop LVGL con poll()/select() per integrazione asincrona:
 *
 *   struct pollfd pfd = { wpa_ctrl_get_fd(mon_ctrl), POLLIN, 0 };
 *   if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN))
 *       // chiama wpa_ctrl_recv()
 * ========================================================================= */

int wpa_ctrl_get_fd(struct wpa_ctrl *ctrl)
{
	return ctrl->s;
}