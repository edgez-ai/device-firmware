#ifndef TEST_NET_IF_H
#define TEST_NET_IF_H
struct net_if { int suspended; };
struct net_if *net_if_get_first_wifi(void);
int net_if_suspend(struct net_if *iface);
int net_if_resume(struct net_if *iface);
#endif
