class OMXPlayerInterface {
    typedef int (*callback_func_t)(OMXReader *reader);

    public:
	OMXPlayerInterface() {
	    callback_func = NULL;
	    loop_func = NULL;
	}

	void set_callback(callback_func_t func) {
	    callback_func = func;
	}

	void set_loop_callback(callback_func_t func) {
	    loop_func = func;
	}

	static OMXPlayerInterface *get_interface();
	int omxplay_event_loop(int argc, char *argv[]);

    private:
	int control_callback(OMXReader *reader) {
	    if (callback_func) return callback_func(reader);
	    return 0;
	}

	int loop_callback(OMXReader *reader) {
	    if (loop_func) return loop_func(reader);
	    return 0;
	}

	callback_func_t callback_func;
	callback_func_t loop_func;
};
