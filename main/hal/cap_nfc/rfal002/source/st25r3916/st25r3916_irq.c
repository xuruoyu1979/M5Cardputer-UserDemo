
/******************************************************************************
  * @attention
  *
  * COPYRIGHT 2016 STMicroelectronics, all rights reserved
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied,
  * AND SPECIFICALLY DISCLAIMING THE IMPLIED WARRANTIES OF MERCHANTABILITY,
  * FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
******************************************************************************/


/*
 *      PROJECT:   ST25R3916 firmware
 *      Revision: 
 *      LANGUAGE:  ISO C99
 */

/*! \file
 *
 *  \author Gustavo Patricio
 *
 *  \brief ST25R3916 Interrupt handling
 *
 */

/*
******************************************************************************
* INCLUDES
******************************************************************************
*/

#include "st25r3916_irq.h"
#include "st25r3916_com.h"
#include "st25r3916_led.h"
#include "st25r3916.h"
#include "rfal_utils.h"

/*
 ******************************************************************************
 * LOCAL DATA TYPES
 ******************************************************************************
 */

/*! Holds current and previous interrupt callback pointer as well as current Interrupt status and mask */
typedef struct
{
    void      (*prevCallback)(void); /*!< call back function for ST25R3916 interrupt          */
    void      (*callback)(void);     /*!< call back function for ST25R3916 interrupt          */
    uint32_t  status;                /*!< latest interrupt status                             */
    uint32_t  mask;                  /*!< Interrupt mask. Negative mask = ST25R3916 mask regs */
} st25r3916Interrupt;


/*
******************************************************************************
* GLOBAL DEFINES
******************************************************************************
*/

/*! Length of the interrupt registers       */
#define ST25R3916_INT_REGS_LEN          ( (ST25R3916_REG_IRQ_TARGET - ST25R3916_REG_IRQ_MAIN) + 1U )

/* LOCAL CHANGE (ESP-IDF port): hard upper bound (ms) for any
 * st25r3916WaitForInterruptsTimed wait, including tmo == 0 (forever) */
#define ST25R3916_WAIT_IRQ_HARD_CAP_MS  200U

/*
******************************************************************************
* GLOBAL VARIABLES
******************************************************************************
*/

static volatile st25r3916Interrupt   st25r3916interrupt; /*!< Instance of ST25R3916 interrupt */

/* LOCAL CHANGE (ESP-IDF port): last raw IRQ register snapshot read by
 * st25r3916CheckForReceivedInterrupts, exported for app-level debug logging */
volatile uint32_t st25r3916irq_last_raw = 0;

/*
******************************************************************************
* GLOBAL FUNCTIONS
******************************************************************************
*/
void st25r3916InitInterrupts( void )
{
    platformIrqST25RPinInitialize();
    platformIrqST25RSetCallback( st25r3916Isr );
    
    
    st25r3916interrupt.callback     = NULL;
    st25r3916interrupt.prevCallback = NULL;
    st25r3916interrupt.status       = ST25R3916_IRQ_MASK_NONE;
    st25r3916interrupt.mask         = ST25R3916_IRQ_MASK_NONE;
}


/*******************************************************************************/
void st25r3916Isr( void )
{
    st25r3916CheckForReceivedInterrupts();
    
    // Check if callback is set and run it
    if( NULL != st25r3916interrupt.callback )
    {
        st25r3916interrupt.callback();
    }
}


/*******************************************************************************/
void st25r3916CheckForReceivedInterrupts( void )
{
    uint8_t  iregs[ST25R3916_INT_REGS_LEN];
    uint32_t irqStatus;
    uint8_t  guard;

    /* Initialize iregs */
    irqStatus = ST25R3916_IRQ_MASK_NONE;
    RFAL_MEMSET( iregs, (int32_t)(ST25R3916_IRQ_MASK_ALL & 0xFFU), ST25R3916_INT_REGS_LEN );

    /* LOCAL CHANGE (ESP-IDF port, no GPIO ISR): read the IRQ registers
     * unconditionally at least once. The original code only reads them while
     * the IRQ pin is high, which silently drops all interrupts when the IRQ
     * GPIO is misconfigured / inverted / not wired. Reading IRQ_MAIN clears
     * the pending bits on chip, so it is always safe. */
    {
        st25r3916ReadMultipleRegisters( ST25R3916_REG_IRQ_MAIN, iregs, ST25R3916_INT_REGS_LEN );

        irqStatus |= (uint32_t)iregs[0];
        irqStatus |= (uint32_t)iregs[1]<<8;
        irqStatus |= (uint32_t)iregs[2]<<16;
        irqStatus |= (uint32_t)iregs[3]<<24;
    }

    /* In case the IRQ is Edge (not Level) triggered read IRQs until done.
     * LOCAL CHANGE: bound the loop. If the IRQ pin is stuck high (floating
     * input, or a pending source that survives the register read) the
     * original unbounded loop spins forever on SPI reads and freezes the
     * whole UI task. Reading the IRQ registers once more per level-triggered
     * pulse is enough; a stuck pin must not deadlock us. */
    guard = 8;
    while( platformGpioIsHigh( ST25R_INT_PORT, ST25R_INT_PIN ) && (guard-- > 0U) )
    {
        st25r3916ReadMultipleRegisters( ST25R3916_REG_IRQ_MAIN, iregs, ST25R3916_INT_REGS_LEN );

        irqStatus |= (uint32_t)iregs[0];
        irqStatus |= (uint32_t)iregs[1]<<8;
        irqStatus |= (uint32_t)iregs[2]<<16;
        irqStatus |= (uint32_t)iregs[3]<<24;
    }

    /* LOCAL CHANGE: keep last raw IRQ registers visible for debugging */
    st25r3916irq_last_raw = irqStatus;

    /* Forward all interrupts, even masked ones to application */
    platformProtectST25RIrqStatus();
    st25r3916interrupt.status |= irqStatus;
    platformUnprotectST25RIrqStatus();

    /* Send an IRQ event to LED handling */
    st25r3916ledEvtIrq( st25r3916interrupt.status );
}


/*******************************************************************************/
void st25r3916ModifyInterrupts(uint32_t clr_mask, uint32_t set_mask)
{
    uint8_t  i;
    uint32_t old_mask;
    uint32_t new_mask;
    

    old_mask = st25r3916interrupt.mask;
    new_mask = ((~old_mask & set_mask) | (old_mask & clr_mask));
    st25r3916interrupt.mask &= ~clr_mask;
    st25r3916interrupt.mask |= set_mask;
    
    for(i=0; i<ST25R3916_INT_REGS_LEN; i++)
    { 
        if( ((new_mask >> (8U*i)) & 0xFFU) == 0U )
        {
            continue;
        }
        
        st25r3916WriteRegister(ST25R3916_REG_IRQ_MASK_MAIN + i, (uint8_t)((st25r3916interrupt.mask>>(8U*i)) & 0xFFU) );
    }
    return;
}


/*******************************************************************************/
uint32_t st25r3916WaitForInterruptsTimed( uint32_t mask, uint16_t tmo )
{
    uint32_t tmrDelay;
    uint32_t status;

    /* LOCAL CHANGE (ESP-IDF port): tmo == 0 means "wait forever" in the
     * original semantics (loop condition `(!expired) || (tmo==0)` is always
     * true). With no GPIO ISR, a missing interrupt would spin forever and
     * freeze the caller (UI task). Cap it to a finite wait. */
    if( tmo == 0U )
    {
        tmo = ST25R3916_WAIT_IRQ_HARD_CAP_MS;
    }
    if( tmo > ST25R3916_WAIT_IRQ_HARD_CAP_MS )
    {
        tmo = ST25R3916_WAIT_IRQ_HARD_CAP_MS;
    }

    tmrDelay = platformTimerCreate( tmo );

    /* Run until specific interrupt has happen or the timer has expired */
    do 
    {
        /* LOCAL CHANGE (ESP-IDF port, no GPIO ISR): actively poll the IRQ pin
         * and read the IRQ registers while pending, otherwise this busy loop
         * would never see any interrupt status update */
        st25r3916CheckForReceivedInterrupts( );
        status = (st25r3916interrupt.status & mask);
    } while( ( (!platformTimerIsExpired( tmrDelay )) || (tmo == 0U)) && (status == 0U) );
    
    platformTimerDestroy( tmrDelay );

    status = st25r3916interrupt.status & mask;
    
    platformProtectST25RIrqStatus();
    st25r3916interrupt.status &= ~status;
    platformUnprotectST25RIrqStatus();
    
    return status;
}


/*******************************************************************************/
uint32_t st25r3916GetInterrupt( uint32_t mask )
{
    uint32_t irqs;

    irqs = (st25r3916interrupt.status & mask);
    if(irqs != ST25R3916_IRQ_MASK_NONE)
    {
        platformProtectST25RIrqStatus();
        st25r3916interrupt.status &= ~irqs;
        platformUnprotectST25RIrqStatus();
    }

    return irqs;
}


/*******************************************************************************/
void st25r3916ClearAndEnableInterrupts( uint32_t mask )
{
    st25r3916GetInterrupt( mask );
    st25r3916EnableInterrupts( mask );
}


/*******************************************************************************/
void st25r3916EnableInterrupts(uint32_t mask)
{
    st25r3916ModifyInterrupts(mask, 0);
}


/*******************************************************************************/
void st25r3916DisableInterrupts(uint32_t mask)
{
    st25r3916ModifyInterrupts(0, mask);
}

/*******************************************************************************/
void st25r3916ClearInterrupts( void )
{
    uint8_t iregs[ST25R3916_INT_REGS_LEN];

    st25r3916ReadMultipleRegisters(ST25R3916_REG_IRQ_MAIN, iregs, ST25R3916_INT_REGS_LEN);

    platformProtectST25RIrqStatus();
    st25r3916interrupt.status = ST25R3916_IRQ_MASK_NONE;
    platformUnprotectST25RIrqStatus();
    return;
}

/*******************************************************************************/
void st25r3916IRQCallbackSet( void (*cb)(void) )
{
    st25r3916interrupt.prevCallback = st25r3916interrupt.callback;
    st25r3916interrupt.callback     = cb;
}

/*******************************************************************************/
void st25r3916IRQCallbackRestore( void )
{
    st25r3916interrupt.callback     = st25r3916interrupt.prevCallback;
    st25r3916interrupt.prevCallback = NULL;
}

