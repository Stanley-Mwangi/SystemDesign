#include "process.hh"

void process4::decrypt()
{
    sc_uint<32> in_value; //Value from the fifo
    sc_uint<32> decrypted_value;

    while ( true )
    {
        // read fifo
        in_value = fifo.read();
        //How long the fifo communication takes
		wait( P3_P4_DELAY, SC_NS );

        //decrypt with key
        decrypted_value = in_value ^ KEY;
        
        //Undo the permutation.
        decrypted_value = ( decrypted_value << 16 ) + (uint16_t)( decrypted_value >> 16 );  

        //pass to testbench
        out_value = decrypted_value; 
        
        wait( P4_LATENCY, SC_NS); 
        //notify that the output is valid
        output_valid.notify();
        wait();
    }
}