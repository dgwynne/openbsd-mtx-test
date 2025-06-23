struct mutex {
	pthread_t	mtx_owner;
};

#include "../mutex_api.h"
