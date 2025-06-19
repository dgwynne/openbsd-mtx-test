struct mutex {
	struct mutex	*mtx_next;
	struct mutex	*mtx_tail;
};

#include "../mutex_api.h"
