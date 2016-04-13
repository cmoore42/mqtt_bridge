CFLAGS = -g

mqtt_bridge:	mqtt_bridge.o
	cc $(CFLAGS) -o mqtt_bridge mqtt_bridge.o -lmosquitto

clean:
	rm -f *.o mqtt_bridge

install:	mqtt_bridge
	cp mqtt_bridge /usr/local/bin
