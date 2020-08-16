# dyndns_daemon

### a dynDNS update daemon

This is a simple deamon for updating a domain name at a dynDNS service.
It's currently developped and tested on my Linux PC.

You also need to checkout https://github.com/megatokio/Libraries. (uppercase 'L')
This project already contains a symlink to '../Libraries'.

The project requires lib curl and lib pthreads.

I use the Qt Creator IDE for development and building.
A Makefile should be staight forward, take a look into .pro file what's needed.

Currently supported:
* IP v4

Planned:
* IP v6


