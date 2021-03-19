#include<unistd.h>
#include<signal.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<netdb.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>

void read_n(int fd,char *buf,int n){
	int i;
	for(i=0;i<n;){
		int x=read(fd,buf+i,n-i);
		if(x<=0){
			perror("reverse_proxy: read");
			exit(-1);
		}
		i+=x;
	}
}
int establish_socks5(int fd,const char *address,unsigned short port){
	char buf[300];
	buf[0]=0x05;
	buf[1]=0x01;
	buf[2]=0x00;
	write(fd,buf,3);
	read_n(fd,buf,2);
	if(buf[0]!=0x05||buf[1]!=0x00){
		fprintf(stderr,"reverse_proxy: Error: Failed to establish a connection\n");
		exit(-1);
	}
	buf[0]=0x05;
	buf[1]=0x01;
	buf[2]=0x00;
	buf[3]=0x01;
	in_addr_t proxy_addr;
	if((proxy_addr=inet_addr(address))<0){
		perror("reverse_proxy: inet_addr");
		exit(-1);
	}
	memcpy(&buf[4],&proxy_addr,4);
	unsigned short proxy_port=htons(port);
	memcpy(&buf[8],&proxy_port,2);
	write(fd,buf,10);
	read_n(fd,buf,5);
	if(buf[0]!=0x05||buf[1]!=0x00){
		fprintf(stderr,"reverse_proxy: Error: Failed to connect to %s:%u\n",address,port);
		exit(-1);
	}
	int n;
	switch(buf[3]){
	case 0x01:
		n=5;
		break;
	case 0x03:
		n=buf[4]+2;
		break;
	case 0x04:
		n=17;
		break;
	default:
		fprintf(stderr,"%s:%u is broken\n",address,port);
		exit(-1);
	}
	read_n(fd,buf,n);
	fprintf(stderr,"Connected to %s:%u\n",address,port);
}
int main(int argc,char *argv[]){
	{
		struct sigaction sa;
		sa.sa_handler=SIG_IGN;
		sa.sa_flags=SA_NOCLDWAIT;
		if(sigaction(SIGCHLD,&sa,NULL)<0){
			perror("reverse_proxy: sigaction");
			exit(-1);
		}
	}
	char proxy_addresses[256][256];
	unsigned short proxy_ports[256];
	int proxy_n=0;
	unsigned short port=12345;

	{
		int i;
		for(i=1;i<argc;i++){
			if(strcmp(argv[i],"-p")==0){
				i++;
				if(i==argc){
					fprintf(stderr,"reverse_proxy: Error: -p <port>\n");
					exit(-1);
				}
				int iport=atoi(argv[i]);
				if(iport<=0||iport>=65536){
					fprintf(stderr,"reverse_proxy: Error: Invalid port\n");
					exit(-1);
				}
				port=iport;
			}
			else{
				if(proxy_n>255){
					fprintf(stderr,"reverse_proxy: Error: Too many args\n");
					exit(-1);
				}
				char *p=strchr(argv[i],':');
				if(p==NULL){
					fprintf(stderr,"reverse_proxy: Error: <domain or IP>:<port>\n");
					exit(-1);
				}
				int len=p-argv[i];
				if(len>255){
					fprintf(stderr,"reverse_proxy: Error: Domain too long\n");
					exit(-1);
				}
				strncpy(proxy_addresses[proxy_n],argv[i],len);
				proxy_addresses[proxy_n][len]='\0';
				int iport=atoi(p+1);
				if(iport<=0||iport>=65536){
					fprintf(stderr,"reverse_proxy: Error: Invalid port\n");
					exit(-1);
				}
				proxy_ports[proxy_n]=iport;
				proxy_n++;
			}
		}
	}
	if(proxy_n==0){
		fprintf(stderr,"Usage: reverse_proxy [-p <port>] [<proxy addess>:<port> ...] <target address>:<port>\n");
		exit(-1);
	}

	int socket_d;
	if((socket_d=socket(AF_INET,SOCK_STREAM,0))<0){
		perror("reverse_proxy: socket");
		exit(-1);
	}
	struct sockaddr_in addr={};
	addr.sin_family=AF_INET;
	addr.sin_addr.s_addr=INADDR_ANY;
	addr.sin_port=htons(port);
	if(bind(socket_d,(struct sockaddr*)&addr,sizeof(addr))<0){
		perror("reverse_proxy: bind");
		exit(-1);
	}
	if(listen(socket_d,10)<0){
		perror("reverse_proxy: listen");
		exit(-1);
	}

	while(1){
		int connection_d;
		if((connection_d=accept(socket_d,NULL,NULL))<0){
			perror("reverse_proxy: accept");
			continue;
		}
		int pid=fork();
		if(pid<0){
			perror("reverse_proxy: fork");
			exit(-1);
		}
		else if(pid==0){
			int first_proxy_d;
			if((first_proxy_d=socket(AF_INET,SOCK_STREAM,0))<0){
				perror("reverse_proxy: socket");
				exit(-1);
			}
			struct sockaddr_in first_proxy_addr={};
			first_proxy_addr.sin_family=AF_INET;
			first_proxy_addr.sin_addr.s_addr=inet_addr(proxy_addresses[0]);
			first_proxy_addr.sin_port=htons(proxy_ports[0]);
			if(connect(first_proxy_d,(struct sockaddr*)&first_proxy_addr,sizeof(first_proxy_addr))<0){
				perror("reverse_proxy: connect");
				fprintf(stderr,"reverse_proxy: Error: Failed to connect to %s:%u\n",proxy_addresses[0],proxy_ports[0]);
				exit(-1);
			}
			fprintf(stderr,"Connected to %s:%u\n",proxy_addresses[0],proxy_ports[0]);

			int i;
			for(i=1;i<proxy_n;i++){
				establish_socks5(first_proxy_d,proxy_addresses[i],proxy_ports[i]);
			}

			fd_set fds,fds2;
			FD_ZERO(&fds2);
			FD_SET(connection_d,&fds2);
			FD_SET(first_proxy_d,&fds2);
			int nfds;
			if(connection_d>first_proxy_d)
				nfds=connection_d;
			else
				nfds=first_proxy_d;
			nfds++;
			char buf[4096];
			while(1){
				memcpy(&fds,&fds2,sizeof(fd_set));
				select(nfds,&fds,NULL,NULL,NULL);
				if(FD_ISSET(connection_d,&fds)){
					int n=read(connection_d,buf,sizeof(buf));
					if(n<0){
						perror("reverse_proxy: read");
						fprintf(stderr,"reverse_proxy: Error: Failed to read client messages\n");
						exit(-1);
					}
					else if(n==0)
						break;
					if(write(first_proxy_d,buf,n)<0){
						perror("reverse_proxy: write");
						exit(-1);
					}
				}
				if(FD_ISSET(first_proxy_d,&fds)){
					int n=read(first_proxy_d,buf,sizeof(buf));
					if(n<0){
						perror("reverse_proxy: read");
						fprintf(stderr,"reverse_proxy: Error: Failed to read proxy messages\n");
						exit(-1);
					}
					else if(n==0)
						break;
					if(write(connection_d,buf,n)<0){
						perror("reverse_proxy: write");
						exit(-1);
					}
				}
			}
			fprintf(stderr,"reverse_proxy: Connection closed\n");
			exit(0);
		}
		else{
			close(connection_d);
			fprintf(stderr,"reverse_proxy: New connection\n");
		}
	}
}
