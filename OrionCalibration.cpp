/*
   OrionCalibration.cpp - A simple self-calibration capability for the Si5351
   using the PPS signal from the GPS and a free Si5351 CLK output fed back to the
   Atmega328p processor via D5, which is the external clock input for Timer1.
 * *** Note D5 must be used for this to work ****

   Timer1 is used as a 16-bit counter along with a 16 bit overflowCounter to literally count
   the pulses generated by the calibration clock. The PPS signal from the GPS provides
   a precise 1 second external clock that generates an interrupt on either pin D2 or D3 (external interrupt pins)
   or alternately via a PinChangeInterrupt on another pin.
   Note that frequency sampled is limited to the Atmega328p clock frequency / 2 as each pluse must exceed the processor
   clock period in order for it to reliably generate an interrupt. We divide by 2.5 for a safety margin.
   That yields a calibration frequency of 3.2 Mhz for an assumed 8Mhz clock.

   Frequency sampling is done for 10 seonds to achieve a 1/10 Hz resolution.
   After each 10 second sample a frequency correction factor is applied using a Huff-n-Puff algorithm.
   Each calibration cycle samples and corrects for 24 iterations so takes approximately 4 minutes.

   Copyright 2019 Michael Babineau, VE3WMB <mbabineau.ve3wmb@gmail.com>


   This sketch  is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.
   Foobar is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License.
   If not, see <http://www.gnu.org/licenses/>.
*/
#include "OrionXConfig.h"
#include "OrionBoardConfig.h"
#include "OrionSi5351.h"
#include "OrionCalibration.h"
#include "OrionSerialMonitor.h"



int32_t old_cal_factor = SI5351A_CLK_FREQ_CORRECTION;
int32_t cal_factor = SI5351A_CLK_FREQ_CORRECTION;

uint64_t measured_rx_freq;
uint64_t target_freq = SI5351_CAL_TARGET_FREQ  // 3.20 MHz, in hundredths of hertz for an 8Mhz processor clock
volatile unsigned int overflowCounter = 0;
volatile unsigned int gpsPPScounter = 0;
volatile bool g_calibration_proceed = false;
volatile bool is_PPS_rising_edge = false; 

// Timer1 is our counter
// 16-bit counter overflows after 65536 counts
// overflowCounter will keep track of how many times we overflow

// Interrupt handler for Timer1 overflow. This is invoked when TCNT1 overflows and TOV1 is set.
// Note that the TOV1 flag is automatically reset through the process of invoking the ISR
ISR(TIMER1_OVF_vect) // Interrupt handler for Timer1 overflow. This is invoked when TCNT1 overflows and TOV1 is set.
{
  overflowCounter++;
}

// Conditional compilation for GPS PPS interrupt handler
#if defined GPS_PPS_ON_D2_OR_D3
  // Interrupt Handler for GPS PPS signal using External Interrupts on D2 or D3
  void PPSinterruptISR()
  {
   gpsPPScounter++;

   if (gpsPPScounter == 1 ) {
     // First PPS pulse received after interrupt enabled
     // enable Frequency counting
     TCNT1  = 0; // Initialize Timer1 counter to 0.
     TIFR1 = (1 << TOV1); //Clear overlow flag in case it is set
     overflowCounter = 0;
   }

    if (gpsPPScounter == 11) { // Ten seconds of counting
     EIMSK = (0 << INT1); // Disable GPS PPS external interrupt (INT1 on PIN D3)- CHANGE THIS to "INT0" IF USING PIN D2
     TCCR1B = 0; // Disable Timer1 Counter

     // We have completed 10 seconds of sampling, this triggers the frequency calculation on RTI
     g_calibration_proceed = true;
    }

  } // end PPSInterruptISR
#else
  // Interrupt Handler for GPS PPS signal using PinChangeInterrupts on A5/PCINT13 typical of U3S clone boards

  //
  // ISR (PCINT0_vect) handles pin change interrupt for D8 to D13 (PORTB)
  // ISR (PCINT1_vect) handles pin change interrupt for A0 to A5 (PORT C)
  // ISR (PCINT1_vect) handles pin change interrupt for A0 to A5 (PORT D) 

  //  A5 uses  PCINT1_vect as an ISR and PCINT13 (PCMSK1 / PCIF1 / PCIE1) 
  ISR (PCINT1_vect) // handle pin change interrupt for A0 to A5 here. This will need modification for use with other pins.
  {
    // PinChange Interrupts don't support triggering on leading or trailing edge (they trigger on both) so we mimic
    // this external interrupt functionality by ignoring every second trigger. 
    // We assume the first pulse is rising and just keep toggling the state back and forth each time the ISR is called,
    // ignoring the trailing edge. Leading or trailing doesn't really matter so long as we are consistent.  
    
   is_PPS_rising_edge = !is_PPS_rising_edge; // toggle the rising edge boolean flag
   
   if (is_PPS_rising_edge == true ) {
    
    gpsPPScounter++;

    if (gpsPPScounter == 1 ) {
      // First PPS pulse received after interrupt enabled
     // enable Frequency counting
     TCNT1  = 0; // Initialize Timer1 counter to 0.
     TIFR1 = (1 << TOV1); //Clear TImer1 overlow flag in case it is set
     overflowCounter = 0;
    }

    if (gpsPPScounter == 11) { // Ten seconds of counting
     PCMSK1 = (0 << PCINT13); // Disable PinChangeInterrupts (GPS PPS interrupt PCINT13 on A5)
     TCCR1B = 0; // Disable Timer1 Counter
     is_PPS_rising_edge = false; 

     // We have completed 10 seconds of sampling, this triggers the frequency calculation on RTI
     g_calibration_proceed = true;
    }

   } // end if (is_PPS_rising_edge)

  }  // end of PCINT1_vect
#endif 


// This resets the Timer1 interrupt
void reset_for_calibration()
{

  // Timer1 Interrupt
  // Timer1 (16 bits) is setup as a frequency counter to sample the Calibration clock
  // Maximum frequency is Fclk_io/2 as sampled pulse duration must be larger than processor clock period(recommended to be < Fclk_io/2.5)
  // Fclk_io is 8 MHz so we are using 3.2 Mhz as the calibration frequency for CAL_CLOCK_NUM
  noInterrupts();
    // Select Normal mode, TCNT1 increments to a max of 0XFFFF, overflows to zero and sets TOV1 (Timer1 overflow flag)
    // Note that the TOV1 flag is automatically reset to 0 by the Timer1 ISR
    TCCR1A = 0;

    TCNT1  = 0; // Initialize Timer1 counter to 0.

    // TCCR1B CS12 =1, CS11=1, CS10=1 means select external clock source on T1 PIN (D5), trigger on rising edge
    // of Si5351 Calibration CLK signal
    TCCR1B = (1 << CS12) | (1 << CS11) | (1 << CS10);

    // Enable Timer1 overflow interrupt - will jump into ISR(TIMER1_OVF_vect) when TOV1 is set
    TIMSK1 = (1 << TOIE1); // Enable Timer1 Overflow Interrupt for now
  interrupts();

  // Turn off the PARK clock
  si5351bx_enable_clk(SI5351A_PARK_CLK_NUM, SI5351_CLK_OFF);

  // Start Calibration clock on target frequency
  si5351bx_setfreq(SI5351A_CAL_CLK_NUM, target_freq);
}

// This initializes both of the Interrupts needed for self-calibration.
void setup_calibration()
{

  // Timer1 Interrupt
  // Timer1 (16 bits) is setup as a frequency counter to sample the Calibration clock
  // Maximum frequency is Fclk_io/2 as sampled pulse duration must be larger than processor clock period(recommended to be < Fclk_io/2.5)
  // Fclk_io is 8 MHz so we are using 3.2 Mhz as the calibration frequency for CAL_CLOCK_NUM
  noInterrupts();
    // Select Normal mode, TCNT1 increments to a max of 0XFFFF, overflows to zero and sets TOV1 (Timer1 overflow flag)
    // Note that the TOV1 flag is automatically reset to 0 by the Timer1 ISR
   TCCR1A = 0;

    TCNT1  = 0; // Initialize Timer1 counter to 0.

    // TCCR1B CS12 =1, CS11=1, CS10=1 means select external clock source on T1 PIN (D5), trigger on rising edge
    // of Si5351 Calibration CLK signal
    TCCR1B = (1 << CS12) | (1 << CS11) | (1 << CS10);

    // Enable Timer1 overflow interrupt - will jump into ISR(TIMER1_OVF_vect) when TOV1 is set
    TIMSK1 = (1 << TOIE1); // Enable Timer1 Overflow Interrupt
  interrupts();

#if defined (GPS_PPS_ON_D2_OR_D3) 
  // Set 1PPS pin D2 or D3 for external interrupt input
  attachInterrupt(digitalPinToInterrupt(GPS_PPS_PIN), PPSinterruptISR, RISING);

  noInterrupts();
    EIMSK = (0 << INT1); // Disable GPS PPS external interupt or now (INT1 on PIN D3) - CHANGE THIS TO "INT0" IF USING PIN D2
  interrupts();
#else
  // We are using PIN Change Interrupts. This will require reconfiguration if using other than Atmega PIN A5 to connect to the GPS PPS PIN
  /* Atmega328p Pin to PinChange Interrupt Register Mappings
  D0    PCINT16 (PCMSK2 / PCIF2 / PCIE2)
  D1    PCINT17 (PCMSK2 / PCIF2 / PCIE2)
  D2    PCINT18 (PCMSK2 / PCIF2 / PCIE2)
  D3    PCINT19 (PCMSK2 / PCIF2 / PCIE2)
  D4    PCINT20 (PCMSK2 / PCIF2 / PCIE2)
  D5    PCINT21 (PCMSK2 / PCIF2 / PCIE2)
  D6    PCINT22 (PCMSK2 / PCIF2 / PCIE2)
  D7    PCINT23 (PCMSK2 / PCIF2 / PCIE2)
  D8    PCINT0  (PCMSK0 / PCIF0 / PCIE0)
  D9    PCINT1  (PCMSK0 / PCIF0 / PCIE0)
  D10   PCINT2  (PCMSK0 / PCIF0 / PCIE0)
  D11   PCINT3  (PCMSK0 / PCIF0 / PCIE0)
  D12   PCINT4  (PCMSK0 / PCIF0 / PCIE0)
  D13   PCINT5  (PCMSK0 / PCIF0 / PCIE0)
  A0    PCINT8  (PCMSK1 / PCIF1 / PCIE1)
  A1    PCINT9  (PCMSK1 / PCIF1 / PCIE1)
  A2    PCINT10 (PCMSK1 / PCIF1 / PCIE1)
  A3    PCINT11 (PCMSK1 / PCIF1 / PCIE1)
  A4    PCINT12 (PCMSK1 / PCIF1 / PCIE1)
  A5    PCINT13 (PCMSK1 / PCIF1 / PCIE1)
  */
  noInterrupts();
    // We are using A5 for GPS_PPS_PIN so PCINT13 (PCMSK1 / PCIF1 / PCIE1)
   PCICR |= (1 << PCIE1);    // [Pin Change Interrupt Control Register] - Enable PinchangeInterrupts for Port C (A5), without disabling PCIE0 or PCIE2
   PCIFR  = (1 << PCIF1);   // [Pin Change Interrupt Flag Register] clear any outstanding interrupts. Counterintuitively writing a 1 clears the flag
   PCMSK1 = (0 << PCINT13); // [Pin Change Mask Register 1] Disable Interrupts for PCINT13 aka PIN A5. 
   is_PPS_rising_edge = false; // Reset our toggle so we can mimic triggering only on rising edge
  interrupts();
#endif


  // Turn off the PARK clock
  si5351bx_enable_clk(SI5351A_PARK_CLK_NUM, SI5351_CLK_OFF);

  // Start Calibration clock on target frequency
  si5351bx_setfreq(SI5351A_CAL_CLK_NUM, target_freq);
}

void do_calibration(unsigned long calibration_step) {
  byte i;
  int timer_counter1 = 0;

  // We do 24 frequency samples at 10 seconds each ( ~ 4 minutes) so the maximum correction is 24 X calibration_step
  for (i = 0; i < 24; i++) {

    // Enable the GPS PPS interrupt, the TIMER1_OVF_vect interrupt handler will enable the Timer1 counter after receiving the first PPS pulse
    // and will then disable everything after 11 pulses (10 seconds of measurement) and set g_calibration_proceed to true.
    noInterrupts();
      g_calibration_proceed = false;
      gpsPPScounter = 0;
      overflowCounter = 0;
    
#if defined (GPS_PPS_ON_D2_OR_D3)  
      // Using External Interrupt on PIN D2 or D3 for GPS PPS 
      EIMSK = (1 << INT1); // Enable GPS PPS external interupt (INT1 on PIN D3) - CHANGE THIS TO "INT0" if using PIN D2
#else
      // Using PinChange Interrupts on Pins other than D2 or D3 
      PCIFR  = (1 << PCIF1);   // [Pin Change Interrupt Flag Register] clear any outstanding interrupts, counterintuitively writing a 1 clears the flag
      PCMSK1 = (1<<PCINT13); // [Pin Change Mask Register 1] Enable Interrupts for PCINT13 on PIN A5 
      is_PPS_rising_edge = false; // Reset our flag so we can mimic external interrupts triggering on rising edge
#endif 

      // Start counter
      TCCR1B = (1 << CS12) | (1 << CS11) | (1 << CS10);
      TIMSK1 = (1 << TOIE1); // Enable Timer1 Overflow Interrupt
    interrupts();

    while (!g_calibration_proceed); // LOOP in place here until the proceed flag is set by PPSinterruptISR after 10 seconds of sampling

    // Done the 10 seconds of sampling, take the count and calculate the frequency.
    noInterrupts();
      timer_counter1 = TCNT1;
    interrupts();

    // We multiply by ten as we are only sampling to a 10th of a Hz but target frequency is expressed in hundredths of Hz
    measured_rx_freq = (timer_counter1 + (65536 * (int64_t)overflowCounter)) * 10ULL;

    old_cal_factor = cal_factor;

    // We apply the Huff-n-Puff method of frequency adjustement by adding or subtracting a fixed calibration step over and over
    // We discard the first measurement (i=0) as it is always low.
    if (i != 0 ) {

      // If measured_rx_freq == target_freq we don't modify the cal_factor.
      if (measured_rx_freq != 0 ) {
        if (measured_rx_freq < target_freq)
          cal_factor = cal_factor - calibration_step;

        if (measured_rx_freq > target_freq)
          cal_factor = cal_factor + calibration_step;
      }
      else {
        // Measured Frequency is Zero so the calibration has failed
        // Todo -  This should be handled by aborting and returning a Fail return code
        swerr(8, 0); // measured_rx_freq is zero so don't modify the calibration factor
      }

      log_debug_Timer1_info(i, overflowCounter, timer_counter1);

      // Log this iteration
      log_calibration(measured_rx_freq, old_cal_factor, cal_factor );

      si5351bx_set_correction(cal_factor); // Update the correction factor and reset the frequency to use it
      si5351bx_setfreq(SI5351A_CAL_CLK_NUM, target_freq);

      delay(10);

    } // end if i!= 0


  } // end for
  
  // Turn off the Calibration clock
  si5351bx_enable_clk(SI5351A_CAL_CLK_NUM, SI5351_CLK_OFF);

  // Turn on the PARK clock
  si5351bx_setfreq(SI5351A_PARK_CLK_NUM, (PARK_FREQ_HZ * 100ULL)); // Turn on Park Clock


} // end do_calibration
