#include <alsa/asoundlib.h>
#include <stdint.h>

#include <bcm_host.h>

#include "OMXReader.h"
#include "omxplayer.h"

#define PACKET_HEADER 0xff
#define PACKET_SIZE 3

#define UART_NAME "hw:1"

#define NUM_OVERLAYS		2
#define OVERLAY_DISPLAY		0
#define OVERLAY_LAYER_HIDDEN	-1
#define OVERLAY_LAYER		1
#define OVERLAY_WIDTH		215
#define OVERLAY_HEIGHT		976
#define OVERLAY_PITCH		ALIGN_UP(OVERLAY_WIDTH * 2, 32)

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
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;
    pthread_cond_t state_changed = PTHREAD_COND_INITIALIZER;
    pthread_cond_t stream_changed = PTHREAD_COND_INITIALIZER;

    int winner;

    int change_state, start_game, change_stream, allow_start;
    enum state_enum state;
    enum state_enum stream_state;
    int stream;

    int start_overlay;
    int finish_overlay;
    int pause_overlay;

    snd_rawmidi_t *output, *input;
    uint8_t controller[NUM_CONTROLLERS];
    uint8_t score[NUM_CONTROLLERS];
    uint8_t cur_instruction;
    control_packet *packet_list;
};

static struct s_game_data game_data;

#define ATTRACT_STREAM		0
#define COUNTDOWN_STREAM	1
#define GAME_STREAM		2
#define WINNER1_STREAM		3
#define WINNER2_STREAM		4

static void state_sleep(int update_state) {
    pthread_mutex_lock(&game_data.lock);
    while (!game_data.change_state)
	pthread_cond_wait(&game_data.state_changed, &game_data.lock);
    if (update_state) {
	game_data.change_state = 0;
    }
    pthread_mutex_unlock(&game_data.lock);
}

static void stream_sleep(void) {
    pthread_mutex_lock(&game_data.lock);
    game_data.change_stream = 0;
    while (!game_data.change_stream)
	pthread_cond_wait(&game_data.stream_changed, &game_data.lock);
    game_data.change_stream = 0;
    pthread_mutex_unlock(&game_data.lock);
}

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
	pthread_mutex_lock(&game_data.lock);
	game_data.cur_instruction = data;
	pthread_mutex_unlock(&game_data.lock);
    } else if (byte_no == 2) {
	/* Add to linked list */
	packet_mem = (control_packet *) malloc(sizeof(struct s_control_packet));
	assert(packet_mem);
	pthread_mutex_lock(&game_data.lock);

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

	pthread_cond_signal(&game_data.data_ready);
	pthread_mutex_unlock(&game_data.lock);
    }
}

static void *uart_func(void *p) {
    while(1) read_uart();

    return NULL;
}

static void *data_func(void *p) {
    control_packet *packet;

    while (1) {
	pthread_mutex_lock(&game_data.lock);
	/* Wait until we have some packets to read */
	while (!game_data.packet_list)
	    pthread_cond_wait(&game_data.data_ready, &game_data.lock);
	/* Unlink one packet */
	packet = game_data.packet_list;
	game_data.packet_list = packet->next;

	switch (packet->instruction >> 4) {
	    case 0x01: /* Digital input */
		if (packet->value == 1 && ((packet->instruction & 0xf) == 0)) {
		    if (game_data.allow_start) {
			game_data.change_state = 1;
			game_data.start_game = 1;
			game_data.allow_start = 0;
			pthread_cond_broadcast(&game_data.state_changed);
		    }
		}
		break;
	    case 0x02: /* Analogue input */
		game_data.controller[packet->instruction & 1] = packet->value;
		break;
	}

	pthread_mutex_unlock(&game_data.lock);

	free(packet);
    }
    return NULL;
}

#define OVERLAY_POWER_L_1   0
#define OVERLAY_POWER_L_2   1
#define OVERLAY_POWER_R_1   2
#define OVERLAY_POWER_R_2   3
#define OVERLAY_BITMAP_L    4
#define OVERLAY_BITMAP_R    5

typedef struct {
    DISPMANX_RESOURCE_HANDLE_T  resource;
    DISPMANX_ELEMENT_HANDLE_T   element;
    uint32_t                    vc_image_ptr;
} dispmanx_element_t;

#define NUM_DISPMANX_ELEMENTS 6
typedef struct {
    DISPMANX_DISPLAY_HANDLE_T   display;
    DISPMANX_MODEINFO_T         info;
    DISPMANX_UPDATE_HANDLE_T    update;
    dispmanx_element_t		elements[NUM_DISPMANX_ELEMENTS];
} dispmanx_data_t;


static void fill_rect(	VC_IMAGE_TYPE_T type, 
			uint16_t *image, 
			int pitch, 
			int x, int y, int w, int h, int val ) {
    int row;
    int col;

    uint16_t *line = image + y * (pitch>>1) + x;

    for ( row = 0; row < h; row++ ) {
        for ( col = 0; col < w; col++ ) {
            line[col] = val;
        }
        line += (pitch>>1);
    }
}

static void create_square(dispmanx_data_t *vars, int index,
		int x, int y, int width, int height, uint8_t opacity,
		uint16_t *data) {
    int ret;
    VC_RECT_T src_rect;
    VC_RECT_T dst_rect;
    VC_IMAGE_TYPE_T type = VC_IMAGE_RGB565;
    int pitch = ALIGN_UP(width*2, 32);
    VC_DISPMANX_ALPHA_T alpha; 
    uint16_t *image;

    /* Setup Opacity */
    alpha.flags = (DISPMANX_FLAGS_ALPHA_T) (
		    DISPMANX_FLAGS_ALPHA_FROM_SOURCE |
		    DISPMANX_FLAGS_ALPHA_FIXED_ALL_PIXELS);
    alpha.opacity = opacity;
    alpha.mask = 0;

    /* Allocate image and convert to VC format */
    if (data == NULL) {
	image = (uint16_t *) calloc( 1, pitch * height );
	assert(image);

	/* Red Rectangle */
	if ((index == OVERLAY_POWER_R_1) || (index == OVERLAY_POWER_R_2))
	    fill_rect( type, image, pitch, 0,  0,    
			width,      height,      0xF800 );
	/* Green Outline */
	if ((index == OVERLAY_POWER_L_1) || (index == OVERLAY_POWER_L_2))
	    fill_rect( type, image, pitch, 0,  0,   
			width,	    height,	0x07E0 );
    } else image = data;

    vars->elements[index].resource = vc_dispmanx_resource_create( type,
                                        width,
                                        height,
                                        &vars->elements[index].vc_image_ptr );
    assert( vars->elements[index].resource );
    vc_dispmanx_rect_set( &dst_rect, 0, 0, width, height);
    ret = vc_dispmanx_resource_write_data(  vars->elements[index].resource,
                                            type,
                                            pitch,
                                            image,
                                            &dst_rect );
    assert( ret == 0 );
    if (data == NULL) free(image);

    /* Render element */
    vars->update = vc_dispmanx_update_start( 0 );
    assert( vars->update );

    vc_dispmanx_rect_set( &src_rect, 0, 0, width << 16, height << 16 );
    vc_dispmanx_rect_set( &dst_rect, x, y, width, height );

    vars->elements[index].element = vc_dispmanx_element_add(    vars->update,
                                                vars->display,
                                                OVERLAY_LAYER_HIDDEN,
                                                &dst_rect,
                                                vars->elements[index].resource,
                                                &src_rect,
                                                DISPMANX_PROTECTION_NONE,
                                                &alpha,
                                                NULL,
                                                (DISPMANX_TRANSFORM_T) 
						    VC_IMAGE_ROT0 );

    ret = vc_dispmanx_update_submit_sync( vars->update );
    assert( ret == 0 );
}

static void destroy_square(dispmanx_data_t *vars, int index) {
    int ret;
    vars->update = vc_dispmanx_update_start( 0 );
    assert( vars->update );
    ret = vc_dispmanx_element_remove( vars->update, 
		    vars->elements[index].element );
    assert( ret == 0 );
    ret = vc_dispmanx_update_submit_sync( vars->update );
    assert( ret == 0 );
    ret = vc_dispmanx_resource_delete( vars->elements[index].resource );
    assert( ret == 0 );
}

static void init_overlay(dispmanx_data_t *vars, int display) {
    int ret;

    vars->display = vc_dispmanx_display_open( display );
    ret = vc_dispmanx_display_get_info( vars->display, &vars->info);
    assert(ret == 0);

}

static void close_overlay(dispmanx_data_t *vars) {
    int ret;

    ret = vc_dispmanx_display_close( vars->display );
    assert( ret == 0 );
}

static void set_visibility(dispmanx_data_t *vars, int index, int visible) {
    int ret;
    vars->update = vc_dispmanx_update_start( 0 );
    assert( vars->update );
    ret = vc_dispmanx_element_change_layer(vars->update, 
		    vars->elements[index].element,
		    visible ? OVERLAY_LAYER : OVERLAY_LAYER_HIDDEN);
    assert( ret == 0 );
    ret = vc_dispmanx_update_submit_sync( vars->update );
    assert( ret == 0 );
}

static void toggle_visibility(dispmanx_data_t *vars, 
			    int index_1, int visible_1,
			    int index_2, int visible_2) {
    int ret;
    vars->update = vc_dispmanx_update_start( 0 );
    assert( vars->update );
    ret = vc_dispmanx_element_change_layer(vars->update, 
		    vars->elements[index_1].element,
		    visible_1 ? OVERLAY_LAYER : OVERLAY_LAYER_HIDDEN);
    assert( ret == 0 );
    ret = vc_dispmanx_element_change_layer(vars->update, 
		    vars->elements[index_2].element,
		    visible_2 ? OVERLAY_LAYER : OVERLAY_LAYER_HIDDEN);
    assert( ret == 0 );
    ret = vc_dispmanx_update_submit_sync( vars->update );
    assert( ret == 0 );
}


static void show_overlays(dispmanx_data_t *dispmanx_data, uint16_t **overlays) {
    int height = dispmanx_data->info.height;
    int width = dispmanx_data->info.width;
    create_square(dispmanx_data, OVERLAY_BITMAP_L, 
		    width * 0.1,
		    (height - OVERLAY_HEIGHT) / 2,
		    OVERLAY_WIDTH, OVERLAY_HEIGHT,
		    120, overlays[0]);
    create_square(dispmanx_data, OVERLAY_BITMAP_R, 
		    width * 0.9 - OVERLAY_WIDTH,
		    (height - OVERLAY_HEIGHT) / 2,
		    OVERLAY_WIDTH, OVERLAY_HEIGHT,
		    120, overlays[1]);
    set_visibility(dispmanx_data, OVERLAY_BITMAP_L, 1);
    set_visibility(dispmanx_data, OVERLAY_BITMAP_R, 1);
}

static void hide_overlays(dispmanx_data_t *dispmanx_data) {
    destroy_square(dispmanx_data, OVERLAY_BITMAP_L);
    destroy_square(dispmanx_data, OVERLAY_BITMAP_R);
}

static void power_bar(dispmanx_data_t *dispmanx_data, int index, int power) {
    int height = dispmanx_data->info.height;
    int width = dispmanx_data->info.width;

    if (power < 1) power = 1;
    if (power > 99) power = 99;

    switch (index) {
	case OVERLAY_POWER_L_1:
	case OVERLAY_POWER_L_2:
	    create_square(dispmanx_data, index, 
		    width * 0.1,
		    (height - OVERLAY_HEIGHT) / 2 +
				OVERLAY_HEIGHT * (100 - power) / 100,
		    OVERLAY_WIDTH, OVERLAY_HEIGHT * power / 100,
		    120, NULL);
	    break;
	case OVERLAY_POWER_R_1:
	case OVERLAY_POWER_R_2:
	    create_square(dispmanx_data, index, 
		    width * 0.9 - OVERLAY_WIDTH,
		    (height - OVERLAY_HEIGHT) / 2 +
				OVERLAY_HEIGHT * (100 - power) / 100,
		    OVERLAY_WIDTH, OVERLAY_HEIGHT * power / 100,
		    120, NULL);
	    break;
    }
}

static int controller_weight(int controller) {
    int raw_val;
    int weighted_val;

    pthread_mutex_lock(&game_data.lock);
    raw_val = game_data.controller[controller & 1];
    if (controller == 1)
	weighted_val = 
		130.0 * (1.0 - exp(-((raw_val - 114)*1.96) / 240.0)) + 10;
    else
	weighted_val = 
		130.0 * (1.0 - exp(-((raw_val - 217)*8.79) / 240.0)) + 10;
    game_data.score[controller & 1] = weighted_val;
    pthread_mutex_unlock(&game_data.lock);

    return weighted_val;
}

static void update_power_bars(dispmanx_data_t *dispmanx_data) {
    int exit = 0;
    int destroy_layer_1 = 0;
    int destroy_layer_2 = 0;
    int current_layer = 1;
    int pause = 0;
    
    int power_level_l;
    int power_level_r;

    while (1) {
	pthread_mutex_lock(&game_data.lock);
	if (game_data.finish_overlay) {
	    game_data.finish_overlay = 0;
	    exit = 1;
	}
	if (game_data.pause_overlay) {
	    game_data.pause_overlay = 0;
	    pause = 1;
	}
	pthread_mutex_unlock(&game_data.lock);

	if (exit) {
	    if (destroy_layer_2) {
                destroy_square(dispmanx_data, OVERLAY_POWER_L_2);
                destroy_square(dispmanx_data, OVERLAY_POWER_R_2);
            } 	
	    if (destroy_layer_1) {
                destroy_square(dispmanx_data, OVERLAY_POWER_L_1);
                destroy_square(dispmanx_data, OVERLAY_POWER_R_1);
            }

	    return;
	}

	if (pause) {
	    usleep(10000);
	    continue;
	}

	power_level_l = controller_weight(0);
	power_level_r = controller_weight(1);

	if (current_layer == 1) {
	    /* Create new layer */
	    power_bar(dispmanx_data, OVERLAY_POWER_L_1, power_level_l);
	    power_bar(dispmanx_data, OVERLAY_POWER_R_1, power_level_r);
	    destroy_layer_1 = 1;

	    toggle_visibility(dispmanx_data,	OVERLAY_POWER_L_2, 0,
						OVERLAY_POWER_L_1, 1);
	    toggle_visibility(dispmanx_data,	OVERLAY_POWER_R_2, 0,
						OVERLAY_POWER_R_1, 1);

	    /* Delete old layer */
	    if (destroy_layer_2) {
		destroy_square(dispmanx_data, OVERLAY_POWER_L_2);
		destroy_square(dispmanx_data, OVERLAY_POWER_R_2);
		destroy_layer_2 = 0;
	    }

	    current_layer = 2;
	} else {
	    /* Create new layer */
	    power_bar(dispmanx_data, OVERLAY_POWER_L_2, power_level_l);
	    power_bar(dispmanx_data, OVERLAY_POWER_R_2, power_level_r);
	    destroy_layer_2 = 1;

	    toggle_visibility(dispmanx_data,	OVERLAY_POWER_L_1, 0,
						OVERLAY_POWER_L_2, 1);
	    toggle_visibility(dispmanx_data,	OVERLAY_POWER_R_1, 0,
						OVERLAY_POWER_R_2, 1);

	    /* Delete old layer */
	    if (destroy_layer_1) {
		destroy_square(dispmanx_data, OVERLAY_POWER_L_1);
		destroy_square(dispmanx_data, OVERLAY_POWER_R_1);
		destroy_layer_1 = 0;
	    }

	    current_layer = 1;
	}
    }
}

static void *overlay_func(void *p) {
    dispmanx_data_t dispmanx_data;
    uint16_t **overlays = (uint16_t **) p;

    state_sleep(0);
    init_overlay(&dispmanx_data, OVERLAY_DISPLAY);

    while (1) {
	/* Wait for correct game stream */
	pthread_mutex_lock(&game_data.lock);
	while (!game_data.start_overlay)
	    pthread_cond_wait(&game_data.stream_changed, &game_data.lock);
	game_data.start_overlay = 0;
	pthread_mutex_unlock(&game_data.lock);

	show_overlays(&dispmanx_data, overlays);

	/* Blocks until we leave game mode */
	update_power_bars(&dispmanx_data);
	
	hide_overlays(&dispmanx_data);
    }

    close_overlay(&dispmanx_data);

    return NULL;
}

static int choose_winner(void) {
    int retval;
    
    if (game_data.score[0] > game_data.score[1]) retval = 0;
    else retval = 1;
    
    return retval;
}

static enum state_enum get_game_state(enum state_enum old_state) {
    switch (old_state) {
	case ATTRACT_MODE:
	    if (game_data.start_game) {
		game_data.start_game = 0;
		return COUNTDOWN_MODE;
	    }
	    else return ATTRACT_MODE;
	case COUNTDOWN_MODE:
	    return GAME_MODE;
	case GAME_MODE:
	    if (game_data.winner) return WINNER1_MODE;
	    else return WINNER2_MODE;
	case WINNER1_MODE:
	case WINNER2_MODE:
	    return ATTRACT_MODE;
    }
    return ATTRACT_MODE;
}

static void set_stream(int stream) {
    pthread_mutex_lock(&game_data.lock);
    game_data.stream = stream;
    pthread_mutex_unlock(&game_data.lock);
}

static void *stream_func(void *p) {
    while (1) {
	pthread_mutex_lock(&game_data.lock);
	game_data.state = get_game_state(game_data.state);
	pthread_mutex_unlock(&game_data.lock);
	switch (game_data.state) {
	    case ATTRACT_MODE:
		set_stream(ATTRACT_STREAM);
		state_sleep(1);
		break;

	    case COUNTDOWN_MODE:
		set_stream(COUNTDOWN_STREAM);
		state_sleep(1);
		break;

	    case GAME_MODE:
		set_stream(GAME_STREAM);
		pthread_mutex_lock(&game_data.lock);
		game_data.start_overlay = 1;
		pthread_mutex_unlock(&game_data.lock);

		stream_sleep();
		sleep(7);

		pthread_mutex_lock(&game_data.lock);
		game_data.pause_overlay = 1;
		game_data.winner = choose_winner();
		pthread_mutex_unlock(&game_data.lock);
		sleep(1);
		
		state_sleep(1);

		pthread_mutex_lock(&game_data.lock);
		game_data.finish_overlay = 1;
		game_data.start_game = 0;
		game_data.allow_start = 1;
		pthread_mutex_unlock(&game_data.lock);
		break;

	    case WINNER1_MODE:
		set_stream(WINNER1_STREAM);
		write_uart(0x11, 0x01);
		state_sleep(1);
		write_uart(0x11, 0x00);
		break;

	    case WINNER2_MODE:
		set_stream(WINNER2_STREAM);
		write_uart(0x12, 0x01);
		state_sleep(1);
		write_uart(0x12, 0x00);
		break;
	}
    }

    return NULL;
}

static int control_callback(OMXReader *reader) {
    int stream;
    static int old_stream;
    int reset = 0;

    pthread_mutex_lock(&game_data.lock);
    stream = game_data.stream;
    pthread_mutex_unlock(&game_data.lock);

    if (stream != old_stream) {
	reader->SetActiveStream(OMXSTREAM_VIDEO, stream);
	reader->SetActiveStream(OMXSTREAM_AUDIO, stream);
	reset = 1;
    }

    old_stream = stream;
    return reset;
}

static int loop_callback(OMXReader *reader) {
    pthread_mutex_lock(&game_data.lock);
    game_data.change_state = 1;
    pthread_cond_broadcast(&game_data.state_changed);
    pthread_mutex_unlock(&game_data.lock);

    usleep(6000000);

    pthread_mutex_lock(&game_data.lock);
    game_data.stream_state = game_data.state;
    game_data.change_stream = 1;
    pthread_cond_broadcast(&game_data.stream_changed);
    pthread_mutex_unlock(&game_data.lock);
    return 1;
}

static int read_overlay_data(const char *filename, uint16_t ***overlays) {
    int i;
    FILE *fp;
    int retval = 0;

    if (!(fp = fopen(filename, "r"))) return -1;

    *overlays = (uint16_t **) malloc(sizeof(uint16_t *) * NUM_OVERLAYS);
    assert(*overlays);

    for (i = 0; i < NUM_OVERLAYS; i++) {
	(*overlays)[i] = (uint16_t *) calloc(1, OVERLAY_PITCH * OVERLAY_HEIGHT);
	assert((*overlays)[i]);
	fread((*overlays)[i], 1, OVERLAY_PITCH * OVERLAY_HEIGHT, fp);
    }

    fclose(fp);
    return retval;
}

#define OMX_PLAYER_ARGS	2
#define OMX_PLAYER_ARG0	"omx_game"
#define OMX_PLAYER_ARG1 "/home/pi/media.mp4"
#define OVERLAY_DATA "/home/pi/overlays.rgb565"
int main(void) {
    int argc = OMX_PLAYER_ARGS;
    char *argv[OMX_PLAYER_ARGS] = { 
	    (char *) OMX_PLAYER_ARG0, (char *) OMX_PLAYER_ARG1 } ;
    OMXPlayerInterface *player;
    pthread_t stream_thread, overlay_thread, uart_thread, data_thread;
    uint16_t **overlays;

    if (read_overlay_data(OVERLAY_DATA, &overlays) < 0) {
	printf("Couldn't open overlay data\n");
	return 1;
    }

    if (setup_uart() < 0) {
	printf("Unable to open uart\n");
	return 1;
    }

    game_data.packet_list = NULL,
    game_data.start_game = 0,
    game_data.allow_start = 1,
    game_data.change_state = 0,
    game_data.change_stream = 0,
    game_data.state = ATTRACT_MODE,
    game_data.stream_state = ATTRACT_MODE,
    game_data.stream = ATTRACT_STREAM;
    game_data.start_overlay = 0;
    game_data.pause_overlay = 0;
    game_data.finish_overlay = 0;

    pthread_create(&stream_thread, NULL, stream_func, NULL);
    pthread_create(&overlay_thread, NULL, overlay_func, overlays);
    pthread_create(&uart_thread, NULL, uart_func, NULL);
    pthread_create(&data_thread, NULL, data_func, NULL);
    
    player = OMXPlayerInterface::get_interface();
    player->set_callback(control_callback);
    player->set_loop_callback(loop_callback);
    player->omxplay_event_loop(argc, argv);

    return 0;
}


