server:
	gcc srv.c -o server -lpaho-mqtt3cs -lcjson

clean:
	rm server *.log

run:
	./server
