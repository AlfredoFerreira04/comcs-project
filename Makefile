server:
	gcc srv.c -o server -lcjson

clean:
	rm server *.log
