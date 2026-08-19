#include <assert.h>
#include <compiler.h>
#include "sound/ct1741.h"

int main(void) {
    assert(ct1741_should_output_audio(0x00, CT1741_DSPMODE_NONE, CT1741_DMAMODE_NONE) == FALSE);
    assert(ct1741_should_output_audio(0x00, CT1741_DSPMODE_DMA, CT1741_DMAMODE_8) == FALSE);
    assert(ct1741_should_output_audio(0xff, CT1741_DSPMODE_NONE, CT1741_DMAMODE_NONE) == FALSE);
    assert(ct1741_should_output_audio(0xff, CT1741_DSPMODE_DMA, CT1741_DMAMODE_8) == TRUE);

    assert(ct1741_should_schedule_dma(0x00, CT1741_DSPMODE_DMA, CT1741_DMAMODE_8, 1024, 1) == FALSE);
    assert(ct1741_should_schedule_dma(0xff, CT1741_DSPMODE_DMA, CT1741_DMAMODE_8, 1024, 1) == TRUE);
    assert(ct1741_should_schedule_dma(0xff, CT1741_DSPMODE_DMA_PAUSE, CT1741_DMAMODE_8, 1024, 1) == FALSE);
    assert(ct1741_should_schedule_dma(0xff, CT1741_DSPMODE_NONE, CT1741_DMAMODE_NONE, 1024, 1) == FALSE);
    return 0;
}
