
void	mtx_init(struct mutex *);
int	mtx_enter_try(struct mutex *);
void	mtx_enter(struct mutex *);
void	mtx_leave(struct mutex *);
