#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
int main(void){
	unsigned char reset[]={0x01,0x03,0x0c,0x00};
	unsigned char lver[]={0x01,0x01,0x10,0x00};
	unsigned char b[64]; int fd,i,got=0,r;
	fd=open("/dev/stpbt",O_RDWR|O_NONBLOCK); if(fd<0){perror("open");return 1;}
	write(fd,reset,4); usleep(200000);
	while(poll(&(struct pollfd){fd,POLLIN,0},1,100)>0){ if(read(fd,b,sizeof b)<=0)break; }
	write(fd,lver,4);
	for(i=0;i<30&&got<12;i++){ struct pollfd p={fd,POLLIN,0}; if(poll(&p,1,100)>0&&(p.revents&POLLIN)){ r=read(fd,b+got,sizeof(b)-got); if(r>0)got+=r; } }
	printf("LocalVersion evt (%d):",got); for(i=0;i<got;i++)printf(" %02x",b[i]); printf("\n");
	close(fd); return 0;
}
