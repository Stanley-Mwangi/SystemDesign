#include "process.hh"

void process3::read_value ()
{
	sc_uint<32> read_enable;
    sc_uint<32> read_value;

	while( true )
	{
        //wait to access shared memory
        do
        {
            wait();
            read_enable = memory->read( ENABLE_INDEX );
        } while ( read_enable == 0 );

        //read the shared Memory
        read_value = memory->read( VALUE_INDEX );        
        //set write enable
        memory->write( 0, ENABLE_INDEX ); 
        // write to fifo
        fifo.write(read_value);
        //How long the fifo communication takes
        wait( P3_P4_DELAY, SC_NS );
        
	}
    
}