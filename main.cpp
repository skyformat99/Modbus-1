#include <iostream>
#include <vector>
#include <bitset>
#include <exception>
#include "modbus.h"
#include "modbus-private.h"
#include "NetServer.h"
#include "config.h"
#include <strings.h>
#include <signal.h>

#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/mapped_region.hpp>



using namespace boost::interprocess;
using namespace std;
using boost::asio::ip::tcp;

//Shared memory object pointer
boost::shared_ptr<shared_memory_object>  sharedMem;
//Each type has a memory map pointer
boost::shared_ptr<mapped_region>    map_ptr[END] ;

char gdir[256] ;
bool bQuit = false ;
bool showCommData =false ;
RAW_COMM_DATA recv_raw ;

Config config ;
vector < boost::shared_ptr<boost::thread> > threads ;

char origin_yk_buf[MAX_YK_NUM*YK_VAR_LEN];

int getCommPortByAddr(uint8_t addr)
{
        for(size_t n = 0;n<config.busLines.size();n++)
        {
            for(size_t m = 0 ;m<config.busLines[n].modules.size() ;m++)
            {
                if(addr == config.busLines[n].modules[m].addr)
                    return n+1 ;
            }
        }
        return -1 ;
}

extern "C" {

void busMonitorSendData(uint8_t *data,uint8_t dataLen)
{
    int com = getCommPortByAddr(data[0]) ;
    RAW_COMM_DATA raw ;
    raw.length = dataLen ;
    raw.isRecv = 0 ;
    memcpy(raw.data,data,dataLen) ;
    {
        boost::mutex::scoped_lock lock(commData_mutex[com]) ;
        if(rawCommDatas.find(com)!=rawCommDatas.end())
            rawCommDatas[com].push_back(raw);
    }
    if(showCommData )
    {
        for(uint8_t i = 0 ; i< dataLen; i++)
        {
            printf("%.2X ", data[i]);
        }
        printf("\n");
    }
}

void busMonitorRecvData(uint8_t * data, uint8_t dataLen,int addNewLine )
{
    if(recv_raw.length+dataLen>=255)
    {
        //某些时候客户端关闭时接收到不完全的数据包，再连接上会把新数据包一直追加
        //在旧缓存后面，容易产生数据越界问题。
        recv_raw.length = 0 ;
        return ;
    }
    memcpy(recv_raw.data+recv_raw.length,data,dataLen);
    recv_raw.length += dataLen ;
    if(addNewLine)
    {
        recv_raw.isRecv = 1 ;
        int com = getCommPortByAddr(recv_raw.data[0]) ;
        if(com>=0&&com<MAX_PORT_NUM)//某些时候串口里面出现干扰字符
        {
            boost::mutex::scoped_lock lock(commData_mutex[com]) ;
            if(rawCommDatas.find(com)!=rawCommDatas.end())
                rawCommDatas[com].push_back(recv_raw);
        }
        recv_raw.length = 0;
    }
//    int com = getCommPortByAddr(data[0]) ;
  //  raw.add = addNewLine ;
    //raw.length = dataLen ;
    //memcpy(raw.data,data,dataLen) ;



    if(showCommData)
    {
        for(uint8_t i = 0 ; i< dataLen; i++)
        {
            printf("%.2X ",data[i]);
        }
        if(addNewLine)
            printf("\n");
    }
}

}

void freeSharedMemroy()
{
    //The remove operation might fail returning false
    //if the shared memory does not exist, the file is
    //open or the file is still memory mapped by other processes:
    sharedMem->remove("MemoryCache");
}


void workerThread(void* p)
{
    Bus *bus = (Bus*)p ;
    modbus_t *modbus;
    //modbus = modbus_new_rtu("/dev/ttyS0",9600,'N',8,1);
    modbus = modbus_new_rtu(bus->sPort,bus->baud,bus->parity,bus->databits,bus->stopbits);
    if( modbus_connect( modbus ) == -1 )
    {
        cout<<"connect "<<bus->sPort<<" error"<<endl ;
        return  ;
    }
    uint8_t dest[2048];
    uint16_t * dest16 = (uint16_t *) dest;
    while(!bQuit)
    {
        int ret ;
        memset( dest, 0, 2048 );



        //test only
//        dest[0] = 0x01 ;dest[1] = 0x02 ;
//        char* p = (char*)map_ptr[1]->get_address();
//        memcpy(p,dest16,10) ;
        //




        for(size_t i = 0 ;i < bus->modules.size();i++)
        {
            for(size_t j = 0 ; j<bus->modules[i].reqs.size();j++)
            {
                modbus_set_slave( modbus, bus->modules[i].addr );
                switch(bus->modules[i].reqs[j].reqType)
                {
                case _FC_READ_COILS:
                    ret = modbus_read_bits( modbus, bus->modules[i].reqs[j].reg, bus->modules[i].reqs[j].num, dest );
                    break;
                case _FC_READ_DISCRETE_INPUTS:
                    ret = modbus_read_input_bits( modbus, bus->modules[i].reqs[j].reg, bus->modules[i].reqs[j].num, dest );
                    break;
                case _FC_READ_HOLDING_REGISTERS:
                    ret = modbus_read_registers( modbus, bus->modules[i].reqs[j].reg, bus->modules[i].reqs[j].num, dest16 );
                    break;
                case _FC_READ_INPUT_REGISTERS:
                    ret = modbus_read_input_registers( modbus, bus->modules[i].reqs[j].reg, bus->modules[i].reqs[j].num, dest16 );
                    break;
                case _FC_WRITE_SINGLE_COIL:
                    {
                        char * p = (char*)map_ptr[yk]->get_address();
                        for(int n = 0 ;n<YK_NUMS ;n++)
                        {
                            if(p[n]!=origin_yk_buf[n])
                            {
                                ret = modbus_write_bit( modbus, bus->modules[i].reqs[j].reg+n,p[n]);
                                origin_yk_buf[n] = p[n];
                                break;
                            }
                        }
                    }
                    break;
                }
                if(bus->modules[i].reqs[j].reqType==_FC_WRITE_SINGLE_COIL)
                {
                    continue ;
                }
                //parse data
                if(ret!=-1)
                {
                    for(size_t n = 0 ; n < bus->modules[i].reqs[j].parses.size();n++)
                    {
                        float f ;
                        char* p = (char*)map_ptr[bus->modules[i].reqs[j].parses[n].powerType-1]->get_address();
                        if(bus->modules[i].reqs[j].parses[n].powerType==1
                                &&(bus->modules[i].reqs[j].reqType==2||bus->modules[i].reqs[j].reqType==1))
                        {
                            //使用1，2命令码时可直接拷贝，返回数据就是byte型
                            memcpy(p+bus->getOffset(yx,i,j,n),dest+bus->modules[i].reqs[j].parses[n].startIndex,
                                   bus->modules[i].reqs[j].parses[n].dataNums);
                        }else
                        {
                            for(int m = 0 ;m< bus->modules[i].reqs[j].parses[n].dataNums ;m++)
                            {
                                switch(bus->modules[i].reqs[j].parses[n].powerType)
                                {
                                    case 1:
                                    {
                                        //使用3，4读取遥信，需要按位转换为byte
                                        bitset<8> b(dest[bus->modules[i].reqs[j].parses[n].startIndex+m/8]);
                                        if(b.test(m%8))
                                            p[bus->getOffset(yx,i,j,n)+m] = 0x01 ;
                                        else
                                            p[bus->getOffset(yx,i,j,n)+m] = 0x00 ;
                                    }
                                    break;
                                case 2:
                                    if(bus->modules[i].reqs[j].parses[n].dataType==3)
                                    {
                                        memcpy(&f,dest+bus->modules[i].reqs[j].parses[n].startIndex+m*4,4);

                                    }
                                    else
                                    {
                                        int itmp ;
                                        if(bus->modules[i].reqs[j].parses[n].dataSize==16)
                                        {
                                            itmp = dest16[bus->modules[i].reqs[j].parses[n].startIndex/2+m] ;
                                        }
                                        else
                                        {
                                            unsigned char *p = (unsigned char*)&itmp ;
                                           // memcpy(&itmp,dest+bus->modules[i].reqs[j].parses[n].startIndex+m*4,4);
                                            memcpy(p,dest+bus->modules[i].reqs[j].parses[n].startIndex+m*4+2,2);
                                            memcpy(p+2,dest+bus->modules[i].reqs[j].parses[n].startIndex+m*4,2);
                                        }
                                        f = itmp ;
                                    }
                                    f = f*bus->modules[i].reqs[j].parses[n].mulVar+bus->modules[i].reqs[j].parses[n].baseVar ;
                                {
                                    int offset = bus->getOffset(yc,i,j,n);
                                    memcpy(p+(offset+m)*4,&f,4);
                                }
                                break;
                                case 3:
                                    if(bus->modules[i].reqs[j].parses[n].dataType==3)
                                    {
                                        memcpy(&f,dest+bus->modules[i].reqs[j].parses[n].startIndex+m*4,4);

                                    }
                                    else
                                    {
                                        int itmp ;
                                        if(bus->modules[i].reqs[j].parses[n].dataSize==16)
                                        {
                                            itmp = dest16[bus->modules[i].reqs[j].parses[n].startIndex/2+m] ;
                                        }
                                        else
                                        {
                                            memcpy(&itmp,dest+bus->modules[i].reqs[j].parses[n].startIndex+m*4,4);
                                        }
                                        f = itmp ;
                                    }
                                    f = f*bus->modules[i].reqs[j].parses[n].mulVar+bus->modules[i].reqs[j].parses[n].baseVar ;
                                {
                                    int offset = bus->getOffset(dd,i,j,n);
                                    memcpy(p+(offset+m)*4,&f,4);
                                }
                                break;
                                }

                            }
                        }
                        //memcpy(p,dest16+bus->modules[i].reqs[j].parses[n].startIndex,bus->modules[i].reqs[j].) ;
                    }
                }

                boost::this_thread::sleep(boost::posix_time::milliseconds(10));
            }
        }
    }
    modbus_close(modbus);
    modbus_free(modbus);
}

//comm data monitor server thread
void monitorThread()
{
    boost::asio::io_service io_service;

    try
    {
        server s(io_service, LISTEN_PORT);
        while(!bQuit)
        {
            io_service.poll();
            boost::this_thread::sleep(boost::posix_time::milliseconds(1)) ;
        }
        io_service.stop();
    }catch(std::exception &e)
    {
        cout<<"Exception:"<<e.what()<<endl ;
    }
}

boost::shared_ptr<shared_memory_object> createSharedMemoryObject()
{
    boost::shared_ptr<shared_memory_object> p(new shared_memory_object(open_or_create,"MemoryCache",read_write)) ;
    return p ;
}

bool initSharedMemory()
{
    try{
        sharedMem = createSharedMemoryObject();
        sharedMem->truncate(TOTAL_MEM_REQ);
        boost::shared_ptr<mapped_region>  yx_ptr(new mapped_region(*sharedMem,read_write,0,MAX_YX_NUM*YX_VAR_LEN)) ;
        boost::shared_ptr<mapped_region>  yc_ptr(new mapped_region(*sharedMem,read_write,MAX_YX_NUM*YX_VAR_LEN,
                                                                   MAX_YC_NUM*YC_VAR_LEN)) ;
        boost::shared_ptr<mapped_region>  dd_ptr(new mapped_region(*sharedMem,read_write,MAX_YX_NUM*YX_VAR_LEN+MAX_YC_NUM*YC_VAR_LEN,
                                                                   MAX_DD_NUM*DD_VAR_LEN)) ;
        boost::shared_ptr<mapped_region>  yk_ptr(new mapped_region(*sharedMem,read_write,MAX_YX_NUM*YX_VAR_LEN+MAX_YC_NUM*YC_VAR_LEN+MAX_DD_NUM*DD_VAR_LEN,
                                                                   MAX_YK_NUM*YK_VAR_LEN)) ;
        map_ptr[yx] = yx_ptr;
        map_ptr[yc] = yc_ptr;
        map_ptr[dd] = dd_ptr ;
        map_ptr[yk] = yk_ptr;
        yx_ptr.reset();
        yc_ptr.reset();
        dd_ptr.reset();
        yk_ptr.reset();

    }catch(interprocess_exception &e)
    {
        char p[256] ;
        memcpy(p,e.what(),256);
        EZLOGGERVLSTREAM(axter::levels(axter::log_always,axter::debug))<<p<<std::endl ;
        return false ;
    }
    memset(origin_yk_buf,0,MAX_YK_NUM*YK_VAR_LEN);

    ////for test only
//    origin_yk_buf[0] = origin_yk_buf[1] = origin_yk_buf[2] = origin_yk_buf[3] = 1 ;
//    char* p = (char*)map_ptr[yk]->get_address();
//    p[0] = p[1] = p[2]=p[3]= 1 ;
    ////

//    try{

//        shared_memory_object sharedMem(open_or_create,"Test",read_write);
//        sharedMem.truncate(256);
//        mapped_region mmap(sharedMem,read_write);

//        memcpy(mmap.get_address(),&n,4);
//        memcpy((char*)(mmap.get_address())+4,&m,4);
//        memcpy((char*)(mmap.get_address())+8,&addr,sizeof(uint16_t));
//        memcpy((char*)(mmap.get_address())+8+sizeof(uint16_t),&nb,sizeof(uint16_t));

//        shared_memory_object::remove("Test");
//    }
//    catch(interprocess_exception &e)
//    {
//        /*shared_memory_object::remove("Test");*/
//        cout<<e.what()<<endl;
//    }
    return true ;
}
void controlHandler(int)
{
    bQuit = true ;
    for(size_t i = 0 ;i < threads.size();i++)
        threads[i]->join();

    freeSharedMemroy();
    EZLOGGERVLSTREAM(axter::log_regularly)<<"Program exited"<<std::endl ;
    exit(1);
}

int main(int argc , char *argv[])
{
#if defined(_WIN32)
    strcpy(gdir,argv[0]);
#else
    readlink("/proc/self/exe",gdir,256);
#endif
    char* p = strrchr(gdir,'/');
    if(p)
    {
        int k = strlen(gdir);
        int n = strlen(p);
        gdir[k-n]='\0';
    }
    //EZLOGGERVLSTREAM(axter::levels(axter::log_regularly,axter::debug))<<"Program started"<<std::endl ;
    EZLOGGERVLSTREAM(axter::log_regularly)<<"Program started"<<std::endl ;
    if(!initSharedMemory())
    {
        EZLOGGERVLSTREAM(axter::log_regularly)<<"内存错误"<<std::endl ;
        return 1;
    }
    signal (SIGINT, controlHandler);
    try{
        config.load();
    }catch(exception &e)
    {
        char p[256] ;
        memcpy(p,e.what(),256);
        cout<<p<<endl ;
        EZLOGGERVLSTREAM(axter::levels(axter::log_regularly,axter::debug))<<p<<std::endl ;
        return 1 ;
    }

    //启动通道数据监视监听线程，默认监听端口10010
    boost::shared_ptr<boost::thread> thread(new boost::thread (monitorThread));
    threads.push_back(thread);

    //根据配置信息分别启动各个串行口的数据采集线程
    for(int i = 0 ;i < config.bus_number ;i++)
    {
        boost::circular_buffer<RAW_COMM_DATA> datas(MAX_CACHE_COMMDATA_NUM) ;
        rawCommDatas[i+1] = datas ;
        boost::shared_ptr<boost::thread> thread(new boost::thread (boost::bind(workerThread,&config.busLines[0])));
        threads.push_back(thread);
    }

    //循环处理用户终端输入，q--退出；d-- 显示报文； p-- 停止显示报文
    //TODO：终端显示报文未同步，多个串口采集时显示报文可能乱序
    char s = 0;
    while(s!='q')
    {
        cin>>s  ;
        if(s =='p')
            showCommData = false ;
        else if(s=='d')
            showCommData = true ;
        boost::this_thread::sleep(boost::posix_time::milliseconds(100));
    }

    bQuit = true ;
    //等待所有工作线程退出
    for(size_t i = 0 ;i < threads.size();i++)
        threads[i]->join();

    freeSharedMemroy();
    EZLOGGERVLSTREAM(axter::log_regularly)<<"Program exited"<<std::endl ;
    return 0;
}

