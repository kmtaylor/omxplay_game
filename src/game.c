//#include <unistd.h>
//#include <fcntl.h>
//#include <assert.h>
#include <alsa/asoundlib.h>
#include <stdint.h>

#include <SDL.h>

#include "omxplayer.h"

#define PACKET_HEADER 0xff
#define PACKET_SIZE 3

#define UART_NAME "hw:1"

typedef struct s_control_packet control_packet;

struct s_control_packet {
    uint8_t instruction;
    uint8_t value;

    control_packet *next;
};

enum state_enum {
    ATTRACT_MODE, GAME_MODE, COUNTDOWN_MODE, WINNER1_MODE, WINNER2_MODE
};

#define NUM_CONTROLLERS 2
struct s_game_data {
    SDL_mutex *lock;
    SDL_cond *data_ready;
    SDL_cond *state_changed;

    control_packet *packet_list;

    uint8_t controller[NUM_CONTROLLERS];

    int start_game;

    enum state_enum state;

    uint8_t cur_instruction;

    snd_rawmidi_t *output, *input;
};

static struct s_game_data game_data = {
    .packet_list = NULL,
    .start_game = 0,
    .state = ATTRACT_MODE,
};

#define ATTRACT_VIDEO_STREAM	0
#define ATTRACT_AUDIO_STREAM	1
#define COUNTDOWN_VIDEO_STREAM	2
#define COUNTDOWN_AUDIO_STREAM	3
#define GAME_VIDEO_STREAM	4
#define GAME_AUDIO_STREAM	5
#define WINNER1_VIDEO_STREAM	6
#define WINNER1_AUDIO_STREAM	7
#define WINNER2_VIDEO_STREAM	8
#define WINNER2_AUDIO_STREAM	9

#define GAME_MODE(mode_name, video_stream, audio_stream) \
    static void mode_name##_mode(VideoState *is) {			    \
	stream_component_close(is, is->last_video_stream);		    \
	stream_component_close(is, is->last_audio_stream);		    \
	stream_component_open(is, video_stream);			    \
	stream_component_open(is, audio_stream);			    \
	stream_seek(is, 0, 0, 0);					    \
    }

#if 0
GAME_MODE(attract, ATTRACT_VIDEO_STREAM, ATTRACT_AUDIO_STREAM)
GAME_MODE(countdown, COUNTDOWN_VIDEO_STREAM, COUNTDOWN_AUDIO_STREAM)
GAME_MODE(game, GAME_VIDEO_STREAM, GAME_AUDIO_STREAM)
GAME_MODE(winner1, WINNER1_VIDEO_STREAM, WINNER1_AUDIO_STREAM)
GAME_MODE(winner2, WINNER2_VIDEO_STREAM, WINNER2_AUDIO_STREAM)
#endif

static int setup_uart(void) {
    return snd_rawmidi_open(&game_data.input, &game_data.output, UART_NAME, 0);
}

static void write_uart(uint8_t instruction, uint8_t value) {
    uint8_t data[PACKET_SIZE];

    data[0] = PACKET_HEADER;
    data[1] = instruction;
    data[2] = value;

    if (snd_rawmidi_write(game_data.output, data, PACKET_SIZE) < 0) {
	printf("error writing\n");
    }
}

static void read_uart(void) {
    int err;
    uint8_t data;
    static int byte_no;
    control_packet *new_packet;
    control_packet *packet_mem;

    if ((err = snd_rawmidi_read(game_data.input, &data, 1)) < 0) {
	return;
    }

    if (data == PACKET_HEADER) byte_no = 0;
    else byte_no++;

    if (byte_no == 1) {
	SDL_LockMutex(game_data.lock);
	game_data.cur_instruction = data;
	SDL_UnlockMutex(game_data.lock);
    } else if (byte_no == 2) {
	/* Add to linked list */
	packet_mem = malloc(sizeof(struct s_control_packet));
	assert(packet_mem);
	SDL_LockMutex(game_data.lock);

	if (!game_data.packet_list) {
	    game_data.packet_list = packet_mem;
	    new_packet = game_data.packet_list;
	} else {
	    new_packet = game_data.packet_list;
	    while (new_packet->next) new_packet = new_packet->next;
	    new_packet->next = packet_mem;
	    new_packet = new_packet->next;
	}
	new_packet->instruction = game_data.cur_instruction;
	new_packet->value = data;
	new_packet->next = NULL;

	SDL_CondSignal(game_data.data_ready);
	SDL_UnlockMutex(game_data.lock);
    }
}

static int uart_func(void *p) {
    while(1) read_uart();

    return 0;
}

static int data_func(void *p) {
    int do_print;
    control_packet *packet;
    while (1) {
	do_print = 0;
	SDL_LockMutex(game_data.lock);
	/* Wait until we have some packets to read */
	while (!game_data.packet_list) {
	    SDL_CondWait(game_data.data_ready, game_data.lock);
	}
	/* Unlink one packet */
	packet = game_data.packet_list;
	game_data.packet_list = packet->next;

	switch (packet->instruction >> 4) {
	    case 0x01: /* Digital input */
		if (packet->value == 1 && ((packet->instruction & 0xf) == 0)) {
		    game_data.start_game = 1;
		    do_print = 1;
		    SDL_CondSignal(game_data.state_changed);
		}
		break;
	    case 0x02: /* Analogue input */
		game_data.controller[packet->instruction & 1] = packet->value;
		do_print = 2;
		break;
	}

	SDL_UnlockMutex(game_data.lock);
	if (do_print == 1) printf("starting\n");
	if (do_print == 2) printf("pot %i\n", packet->value);

	free(packet);
    }
    return 0;
}

#if 0
/* Return index of lower left pixel of bar graph, from percentage screen size */
static int bar_origin(SDL_Overlay *overlay, int accross, int down, int div) {
    int line_no, pixel_address;
    line_no = (down/100.0) * overlay->h / div;
    pixel_address = line_no * overlay->w / div;
    pixel_address += (accross/100.0) * overlay->w / div;
    return pixel_address;
}

/* Return bar width in pixels from percentage of screen size */
static int bar_width(SDL_Overlay *overlay, int accross) {
    return (accross/100.0) * overlay->w;
}

/* Return bar height in lines from percentage of screen size */
static int bar_height(SDL_Overlay *overlay, int up) {
    return  (up/100.0) * overlay->h;
}

static double shape(int i, int width) {
    if (i > width*7.0/8.0) i = width - i; 
    if (i < width/8.0) return i / (width/8.0);
    return 1;
}

static void draw_horizontal_line(SDL_Overlay *overlay, int origin, int width,
		uint8_t colour, int layer) {
    int i;
    for (i = 0; i < width; i++) {
	if (layer == 0)
	    overlay->pixels[layer][i + origin] = colour * shape(i, width);
	else
	    overlay->pixels[layer][i + origin] = colour;
    }
}

static int controller_weight(int controller) {
    int raw_val;

    SDL_LockMutex(game_data.lock);
    raw_val = game_data.controller[controller & 1];
    SDL_UnlockMutex(game_data.lock);

    return 100.0 * (1.0 - exp(-raw_val / 80.0));
}

#define LEFT_BAR_ORIGIN_ACCROSS	    3
#define LEFT_BAR_ORIGIN_DOWN	    98
#define LEFT_BAR_WIDTH		    12
#define RIGHT_BAR_ORIGIN_ACCROSS    85
#define RIGHT_BAR_ORIGIN_DOWN	    98
#define RIGHT_BAR_WIDTH		    12
#define BAR_HEIGHT		    94
#define BAR_COLOUR_RED		    128
#define BAR_COLOUR_GREEN	    0
#define BAR_COLOUR_BLUE		    0
void frame_modify_hook(SDL_Overlay *overlay) {
    int i;
    int origin, pixel_height, pixel_width;

    if (game_data.state != GAME_MODE) return;

    SDL_LockYUVOverlay (overlay);

    /* Left player bar */
    pixel_height = bar_height(overlay, controller_weight(0));
    for (i = 0; i < pixel_height; i++) {
	origin = bar_origin(overlay, LEFT_BAR_ORIGIN_ACCROSS,
			    LEFT_BAR_ORIGIN_DOWN, 1);
	origin -= i * overlay->w;
	pixel_width = bar_width(overlay, LEFT_BAR_WIDTH);
	pixel_width *= (1.0) * i / bar_height(overlay, BAR_HEIGHT);
	draw_horizontal_line(overlay, origin, pixel_width,
			RGB_TO_Y(BAR_COLOUR_BLUE, BAR_COLOUR_GREEN,
				BAR_COLOUR_RED), 0);
    }
    for (i = 0; i < pixel_height / 2; i++) {
	origin = bar_origin(overlay, LEFT_BAR_ORIGIN_ACCROSS,
			    LEFT_BAR_ORIGIN_DOWN, 2);
	origin -= i * overlay->w / 2;
	pixel_width = bar_width(overlay, LEFT_BAR_WIDTH) / 2;
	pixel_width *= (2.0) * i / bar_height(overlay, BAR_HEIGHT);
	pixel_width += 1;
	draw_horizontal_line(overlay, origin, pixel_width,
			RGB_TO_U(BAR_COLOUR_BLUE, BAR_COLOUR_GREEN,
				BAR_COLOUR_RED, 0), 1);
	draw_horizontal_line(overlay, origin, pixel_width,
			RGB_TO_V(BAR_COLOUR_BLUE, BAR_COLOUR_GREEN,
				BAR_COLOUR_RED, 0), 2);
    }

    /* Right player bar */
    pixel_height = bar_height(overlay, controller_weight(1));
    for (i = 0; i < pixel_height; i++) {
	origin = bar_origin(overlay, RIGHT_BAR_ORIGIN_ACCROSS,
			    RIGHT_BAR_ORIGIN_DOWN, 1);
	origin -= i * overlay->w;
	pixel_width = bar_width(overlay, RIGHT_BAR_WIDTH);
	draw_horizontal_line(overlay, origin, pixel_width,
			RGB_TO_Y_CCIR(BAR_COLOUR_BLUE, BAR_COLOUR_GREEN,
				BAR_COLOUR_RED), 0);
    }
    for (i = 0; i < pixel_height / 2; i++) {
	origin = bar_origin(overlay, RIGHT_BAR_ORIGIN_ACCROSS,
			    RIGHT_BAR_ORIGIN_DOWN, 2);
	origin -= i * overlay->w / 2;
	pixel_width = bar_width(overlay, RIGHT_BAR_WIDTH) / 2;
	draw_horizontal_line(overlay, origin, pixel_width,
			RGB_TO_U_CCIR(BAR_COLOUR_BLUE, BAR_COLOUR_GREEN,
				BAR_COLOUR_RED, 0), 1);
	draw_horizontal_line(overlay, origin, pixel_width,
			RGB_TO_V_CCIR(BAR_COLOUR_BLUE, BAR_COLOUR_GREEN,
				BAR_COLOUR_RED, 0), 2);
    }

    SDL_UnlockYUVOverlay (overlay);
}
#endif

static void sleep_mode(void) {
    int timeout = 0;
    switch (game_data.state) {
	case ATTRACT_MODE:
	    SDL_LockMutex(game_data.lock);
	    while (!game_data.start_game && !timeout)
		timeout = SDL_CondWaitTimeout(game_data.state_changed,
				game_data.lock, 18 * 1000);
	    SDL_UnlockMutex(game_data.lock);
	    break;
	case COUNTDOWN_MODE:
	    sleep(8);
	    break;
	case GAME_MODE:
	    sleep(5);
	    break;
	case WINNER1_MODE:
	    sleep(5);
	    break;
	case WINNER2_MODE:
	    sleep(5);
	    break;
    }
}

static int choose_winner(void) {
    int retval;
    
    SDL_LockMutex(game_data.lock);
    
    if (game_data.controller[0] > game_data.controller[1]) retval = 0;
    else retval = 1;

    SDL_UnlockMutex(game_data.lock);
    
    return retval;
}

static enum state_enum get_game_state(enum state_enum old_state) {
    switch (old_state) {
	case ATTRACT_MODE:
	    SDL_LockMutex(game_data.lock);
	    if (game_data.start_game) {
		game_data.start_game = 0;
		SDL_UnlockMutex(game_data.lock);
		return COUNTDOWN_MODE;
	    } else {
		SDL_UnlockMutex(game_data.lock);
		return ATTRACT_MODE;
	    }
	case COUNTDOWN_MODE:
	    return GAME_MODE;
	case GAME_MODE:
	    if (choose_winner()) return WINNER1_MODE;
	    else return WINNER2_MODE;
	case WINNER1_MODE:
	case WINNER2_MODE:
	    return ATTRACT_MODE;
    }
    return ATTRACT_MODE;
}

#if 0
static int stream_func(void *is_p) {
    VideoState *is = (VideoState *) is_p;

    sleep_mode();

    while (1) {
	game_data.state = get_game_state(game_data.state);
	switch (game_data.state) {
	    case ATTRACT_MODE:
		attract_mode(is);
		sleep_mode();
		break;

	    case COUNTDOWN_MODE:
		countdown_mode(is);
		sleep_mode();
		break;

	    case GAME_MODE:
		game_mode(is);
		SDL_LockMutex(game_data.lock);
		game_data.start_game = 0;
		SDL_UnlockMutex(game_data.lock);
		/* draw bar graphs (in a separate thread) */
		sleep_mode();
		break;

	    case WINNER1_MODE:
		winner1_mode(is);
		write_uart(0x11, 0x01);
		sleep_mode();
		write_uart(0x11, 0x00);
		break;

	    case WINNER2_MODE:
		winner2_mode(is);
		write_uart(0x12, 0x01);
		sleep_mode();
		write_uart(0x12, 0x00);
		break;
	}
    }

    return 0;
}
#endif

int main(int argc, char **argv) {
    //VideoState *is;

    //wanted_stream[AVMEDIA_TYPE_AUDIO] = ATTRACT_AUDIO_STREAM;
    //wanted_stream[AVMEDIA_TYPE_VIDEO] = ATTRACT_VIDEO_STREAM;

#if 0
    if (setup_uart() < 0) {
	printf("Unable to open uart\n");
	return 1;
    }
#endif

    //is = ffplay_init("/home/pi/ffplay_game/resource/media.mpg");

    game_data.lock = SDL_CreateMutex();
    game_data.data_ready = SDL_CreateCond();
    game_data.state_changed = SDL_CreateCond();

    //SDL_CreateThread(stream_func, NULL);
    //SDL_CreateThread(uart_func, NULL);
    //SDL_CreateThread(data_func, NULL);

    omxplay_event_loop(argc, argv);

    return 0;
}
