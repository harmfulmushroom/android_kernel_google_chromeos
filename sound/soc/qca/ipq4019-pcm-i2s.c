/* Copyright (c) 2016, The Linux Foundation. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <linux/init.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <asm/dma.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/initval.h>
#include <sound/control.h>
#include <sound/pcm_params.h>

#include "ipq4019-adss.h"
#include "ipq4019-mbox.h"

struct ipq4019_pcm_rt_priv {
	int channel;
	struct device *dev;
	struct ipq4019_mbox_desc *last_played;
	unsigned int processed_size;
	u32 period_size;
	u32 curr_pos;
};

static struct snd_pcm_hardware ipq4019_pcm_hardware_playback = {
	.info			=	SNDRV_PCM_INFO_MMAP |
					SNDRV_PCM_INFO_BLOCK_TRANSFER |
					SNDRV_PCM_INFO_MMAP_VALID |
					SNDRV_PCM_INFO_INTERLEAVED |
					SNDRV_PCM_INFO_PAUSE |
					SNDRV_PCM_INFO_RESUME,
	.formats		=	SNDRV_PCM_FMTBIT_S16 |
					SNDRV_PCM_FMTBIT_S32,
	.rates			=	RATE_16000_96000,
	.rate_min		=	FREQ_16000,
	.rate_max		=	FREQ_96000,
	.channels_min		=	CH_STEREO,
	.channels_max		=	CH_STEREO,
	.buffer_bytes_max	=	IPQ4019_I2S_BUFF_SIZE,
	.period_bytes_max	=	IPQ4019_I2S_BUFF_SIZE / 2,
	.period_bytes_min	=	IPQ4019_I2S_PERIOD_BYTES_MIN,
	.periods_min		=	IPQ4019_I2S_NO_OF_PERIODS,
	.periods_max		=	IPQ4019_I2S_NO_OF_PERIODS,
	.fifo_size		=	0,
};

static struct snd_pcm_hardware ipq4019_pcm_hardware_capture = {
	.info			=	SNDRV_PCM_INFO_MMAP |
					SNDRV_PCM_INFO_BLOCK_TRANSFER |
					SNDRV_PCM_INFO_MMAP_VALID |
					SNDRV_PCM_INFO_INTERLEAVED,
	.formats		=	SNDRV_PCM_FMTBIT_S16 |
					SNDRV_PCM_FMTBIT_S32,
	.rates			=	RATE_16000_96000,
	.rate_min		=	FREQ_16000,
	.rate_max		=	FREQ_96000,
	.channels_min		=	CH_STEREO,
	.channels_max		=	CH_STEREO,
	.buffer_bytes_max	=	IPQ4019_I2S_BUFF_SIZE,
	.period_bytes_max	=	IPQ4019_I2S_BUFF_SIZE / 2,
	.period_bytes_min	=	IPQ4019_I2S_PERIOD_BYTES_MIN,
	.periods_min		=	IPQ4019_I2S_NO_OF_PERIODS,
	.periods_max		=	IPQ4019_I2S_NO_OF_PERIODS,
	.fifo_size		=	0,
};

static size_t ip4019_dma_buffer_size(struct snd_pcm_hardware *pcm_hw)
{
	return (pcm_hw->buffer_bytes_max +
		(pcm_hw->periods_min * sizeof(struct ipq4019_mbox_desc)));
}

static struct device *ss2dev(struct snd_pcm_substream *substream)
{
	return substream->pcm->card->dev;
}

/*
 * The MBOX descriptors and buffers should lie within the same 256MB
 * region. Because, the buffer address pointer (in the descriptor structure)
 * and descriptor base address pointer register share the same MSB 4 bits
 * which is configured in MBOX DMA Policy register.
 *
 * Hence ensure that the entire allocated region falls in a 256MB region.
 */
static int ipq4019_mbox_buf_is_aligned(void *c_ptr, ssize_t size)
{
	u32 ptr = (u32)c_ptr;

	return (ptr & 0xF0000000) == ((ptr + size - 1) & 0xF0000000);
}

static int ipq4019_pcm_preallocate_dma_buffer(struct snd_pcm *pcm,
						int stream)
{
	struct snd_pcm_substream *substream = pcm->streams[stream].substream;
	struct snd_pcm_hardware *pcm_hw;
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	size_t size;
	u8 *area;
	dma_addr_t addr;

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		pcm_hw = &ipq4019_pcm_hardware_playback;
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		pcm_hw = &ipq4019_pcm_hardware_capture;
		break;
	default:
		dev_err(ss2dev(substream), "Invalid stream: %d\n",
							substream->stream);
		return -EINVAL;
	}

	size = ip4019_dma_buffer_size(pcm_hw);
	buf->dev.type = SNDRV_DMA_TYPE_DEV;
	buf->dev.dev = pcm->card->dev;
	buf->private_data = NULL;

	/*
	 * |<--   buffers             -->|<-- desc  -->|
	 * +----+----+----+----+----+----+-+-+-+-+-+-+-+
	 * |    |    |    |    |    |    | | | | | | | |
	 * |    |    |    |    |    |    | | | | | | | |
	 * |    |    |    |    |    |    | | | | | | | |
	 * |    |    |    |    |    |    | | | | | | | |
	 * |    |    |    |    |    |    | | | | | | | |
	 * +----+----+----+----+----+----+-+-+-+-+-+-+-+
	 * ^    ^                        | |  . . .
	 * |    |                        | |
	 * +----|------------------------+ |
	 *	+--------------------------+
	 */

	/*
	 * Currently payload uses uncached memory.
	 * TODO: Eventually we will move to cached memory for payload
	 * and dma_map_single() will be used for Invalidating/Flushing
	 * the buffers.
	 */

	area = dma_alloc_coherent(pcm->card->dev, size, &addr, GFP_KERNEL);

	if (!area) {
		dev_info(ss2dev(substream), "Alloc coherent memory failed\n");
		return -ENOMEM;
	}

	if (!ipq4019_mbox_buf_is_aligned(area, size)) {
		dev_info(ss2dev(substream),
			 "First allocation %p not within 256M region\n",
			 area);

		buf->area = dma_alloc_coherent(pcm->card->dev, size,
						&buf->addr, GFP_KERNEL);
		/*
		 * If we are here, the previously allocated buffer is not
		 * usable for the driver. Have to free it anyway regardless
		 * of the success/failure of the second allocation.
		 */
		dma_free_coherent(pcm->card->dev, size, area, addr);
		if (!buf->area) {
			dev_info(ss2dev(substream),
				 "Second Alloc coherent memory failed\n");
			return -ENOMEM;
		}
	} else {
		buf->area = area;
		buf->addr = addr;
	}

	buf->bytes = pcm_hw->buffer_bytes_max;

	return 0;
}

static void ipq4019_pcm_free_dma_buffer(struct snd_pcm *pcm, int stream)
{
	struct snd_pcm_substream *substream;
	struct snd_pcm_hardware *pcm_hw;
	struct snd_dma_buffer *buf;
	size_t size;

	substream = pcm->streams[stream].substream;
	buf = &substream->dma_buffer;

	switch (stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		pcm_hw = &ipq4019_pcm_hardware_playback;
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		pcm_hw = &ipq4019_pcm_hardware_capture;
		break;
	}

	size = ip4019_dma_buffer_size(pcm_hw);

	dma_free_coherent(pcm->card->dev, size, buf->area, buf->addr);

	buf->addr = 0;
}

static irqreturn_t ipq4019_pcm_irq(int intrsrc, void *data)
{
	struct snd_pcm_substream *substream = data;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq4019_pcm_rt_priv *pcm_rtpriv = runtime->private_data;

	pcm_rtpriv->curr_pos =
		ipq4019_mbox_get_played_offset(pcm_rtpriv->channel);

	snd_pcm_period_elapsed(substream);

	return IRQ_HANDLED;
}

static snd_pcm_uframes_t ipq4019_pcm_i2s_pointer(
				struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq4019_pcm_rt_priv *pcm_rtpriv = runtime->private_data;

	return bytes_to_frames(runtime, pcm_rtpriv->curr_pos);
}

static int ipq4019_pcm_i2s_copy(struct snd_pcm_substream *substream, int chan,
				snd_pcm_uframes_t hwoff, void __user *ubuf,
				snd_pcm_uframes_t frames)
{
	struct snd_dma_buffer *buf = &substream->dma_buffer;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq4019_pcm_rt_priv *pcm_rtpriv = runtime->private_data;
	char *hwbuf;
	u32 offset, size;

	offset = frames_to_bytes(runtime, hwoff);
	size = frames_to_bytes(runtime, frames);

	hwbuf = buf->area + offset;

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		if (copy_from_user(hwbuf, ubuf, size))
			return -EFAULT;
	} else if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		if (copy_to_user(ubuf, hwbuf, size))
			return -EFAULT;
	}

	ipq4019_mbox_desc_own(pcm_rtpriv->channel, offset / size, 1);

	ipq4019_mbox_dma_resume(pcm_rtpriv->channel);

	return 0;
}

static int ipq4019_pcm_i2s_mmap(struct snd_pcm_substream *substream,
				struct vm_area_struct *vma)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	return dma_mmap_coherent(substream->pcm->card->dev, vma,
		runtime->dma_area, runtime->dma_addr, runtime->dma_bytes);
}

static int ipq4019_pcm_hw_free(struct snd_pcm_substream *substream)
{
	snd_pcm_set_runtime_buffer(substream, NULL);
	return 0;
}


static int ipq4019_pcm_i2s_prepare(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq4019_pcm_rt_priv *pcm_rtpriv = runtime->private_data;
	u32 ret;

	ret = ipq4019_mbox_dma_prepare(pcm_rtpriv->channel);
	if (ret) {
		dev_err(ss2dev(substream),
			"Error in dma prepare: channel: %d ret: %d\n",
			pcm_rtpriv->channel, ret);
		return ret;
	}

	pcm_rtpriv->last_played = NULL;

	return 0;
}

static int ipq4019_pcm_i2s_close(struct snd_pcm_substream *substream)
{
	struct ipq4019_pcm_rt_priv *pcm_rtpriv =
				substream->runtime->private_data;
	u32 ret;

	ret = ipq4019_mbox_dma_release(pcm_rtpriv->channel);
	if (ret)
		dev_err(ss2dev(substream),
			"Error in dma release. ret: %d\n", ret);

	kfree(pcm_rtpriv);

	return 0;
}

static int ipq4019_pcm_i2s_trigger(struct snd_pcm_substream *substream, int cmd)
{
	int ret;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq4019_pcm_rt_priv *pcm_rtpriv = runtime->private_data;
	u32 desc_duration;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		ret = ipq4019_mbox_dma_start(pcm_rtpriv->channel);
		if (ret) {
			dev_err(ss2dev(substream),
				"Error in dma start. ret: %d\n", ret);
		}
		break;
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = ipq4019_mbox_dma_resume(pcm_rtpriv->channel);
		if (ret) {
			dev_err(ss2dev(substream),
				"Error in dma resume. ret: %d\n", ret);
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		/*
		 * For e.g. the number of bytes needed to represent 1 second
		 * worth of audio data for sampling frequency, bit width, stereo
		 * combination of 16KHz, 32-bits and stereo, the calculation is
		 * as follows
		 *
		 * For 1 second,
		 *	16KHz * 32 bits * 2 (left & right channel of stereo)
		 *	= 16000 * 4 bytes * 2
		 *	= 128000 bytes
		 *
		 * Hence the duration will be
		 *	desc_buffer_size_in_bytes / 128000 * 1 sec
		 */
		desc_duration =
			frames_to_bytes(runtime, runtime->period_size) * 1000 /
				(runtime->rate *
				 DIV_ROUND_UP(runtime->sample_bits, 8) *
				 runtime->channels);

		dev_dbg(ss2dev(substream),
			"period_size:%u rate:%u sample_bits:%u channels:%u desc_delay:%u\n",
			frames_to_bytes(runtime, runtime->period_size),
			runtime->rate, runtime->sample_bits, runtime->channels,
			desc_duration);

		ret = ipq4019_mbox_dma_stop(pcm_rtpriv->channel, desc_duration);
		if (ret) {
			dev_err(ss2dev(substream),
				"Error in dma stop. ret: %d\n", ret);
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int ipq4019_pcm_i2s_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *hw_params)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq4019_pcm_rt_priv *pcm_rtpriv = runtime->private_data;
	int ret;
	unsigned int period_size, sample_size, sample_rate, frames, channels;

	ret = ipq4019_mbox_form_ring(pcm_rtpriv->channel,
			substream->dma_buffer.addr,
			substream->dma_buffer.area,
			params_period_bytes(hw_params),
			params_buffer_bytes(hw_params));
	if (ret) {
		dev_dbg(ss2dev(substream),
			"Error dma form ring ret: %d\n", ret);
		return ret;
	}

	period_size = params_period_bytes(hw_params);
	sample_size = snd_pcm_format_size(params_format(hw_params), 1);
	sample_rate = params_rate(hw_params);
	channels = params_channels(hw_params);
	frames = period_size / (sample_size * channels);

	pcm_rtpriv->period_size = params_period_bytes(hw_params);

	snd_pcm_set_runtime_buffer(substream, &substream->dma_buffer);

	runtime->dma_bytes = params_buffer_bytes(hw_params);
	return 0;
}

static int ipq4019_pcm_i2s_open(struct snd_pcm_substream *substream)
{
	int ret;
	struct snd_pcm_runtime *runtime = substream->runtime;
	struct ipq4019_pcm_rt_priv *pcm_rtpriv;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *dai = rtd->cpu_dai;
	u32 intf = dai->driver->id;

	pcm_rtpriv = kmalloc(sizeof(struct ipq4019_pcm_rt_priv), GFP_KERNEL);
	if (!pcm_rtpriv)
		return -ENOMEM;

	dev_dbg(ss2dev(substream), "%s: 0x%xB allocated at 0x%08x\n",
			__func__, sizeof(*pcm_rtpriv), (u32) pcm_rtpriv);
	pcm_rtpriv->last_played = NULL;
	pcm_rtpriv->dev = substream->pcm->card->dev;
	pcm_rtpriv->channel = ipq4019_get_mbox_id(substream, intf);
	pcm_rtpriv->curr_pos = 0;
	substream->runtime->private_data = pcm_rtpriv;

	switch (substream->stream) {
	case SNDRV_PCM_STREAM_PLAYBACK:
		runtime->dma_bytes =
			ipq4019_pcm_hardware_playback.buffer_bytes_max;
		snd_soc_set_runtime_hwparams(substream,
				&ipq4019_pcm_hardware_playback);
		break;
	case SNDRV_PCM_STREAM_CAPTURE:
		runtime->dma_bytes =
				ipq4019_pcm_hardware_capture.buffer_bytes_max;
		snd_soc_set_runtime_hwparams(substream,
					&ipq4019_pcm_hardware_capture);
		break;
	default:
		dev_err(ss2dev(substream), "Invalid stream: %d\n",
			substream->stream);
		ret = -EINVAL;
		goto error;
	}

	ret = ipq4019_mbox_dma_init(pcm_rtpriv->dev,
		pcm_rtpriv->channel, ipq4019_pcm_irq, substream);
	if (ret) {
		dev_err(ss2dev(substream),
			"Error initializing dma. ret: %d\n", ret);
		goto error;
	}

	ret = snd_pcm_hw_constraint_integer(runtime,
			SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0) {
		dev_err(ss2dev(substream),
			"snd_pcm_hw_constraint_integer failed ret: %d\n",
			ret);
		goto error_hw_const;
	}

	return 0;
error_hw_const:
	ipq4019_mbox_dma_deinit(pcm_rtpriv->channel);
error:
	kfree(pcm_rtpriv);
	return ret;
}

static struct snd_pcm_ops ipq4019_asoc_pcm_i2s_ops = {
	.open		= ipq4019_pcm_i2s_open,
	.hw_params	= ipq4019_pcm_i2s_hw_params,
	.hw_free	= ipq4019_pcm_hw_free,
	.trigger	= ipq4019_pcm_i2s_trigger,
	.ioctl		= snd_pcm_lib_ioctl,
	.close		= ipq4019_pcm_i2s_close,
	.prepare	= ipq4019_pcm_i2s_prepare,
	.mmap		= ipq4019_pcm_i2s_mmap,
	.pointer	= ipq4019_pcm_i2s_pointer,
	.copy		= ipq4019_pcm_i2s_copy,
};

static void ipq4019_asoc_pcm_i2s_free(struct snd_pcm *pcm)
{
	ipq4019_pcm_free_dma_buffer(pcm, SNDRV_PCM_STREAM_PLAYBACK);
	ipq4019_pcm_free_dma_buffer(pcm, SNDRV_PCM_STREAM_CAPTURE);
}

static int ipq4019_asoc_pcm_i2s_new(struct snd_soc_pcm_runtime *prtd)
{
	struct snd_card *card = prtd->card->snd_card;
	struct snd_pcm *pcm = prtd->pcm;
	int ret = 0, pback = 0;

	if (!card->dev->coherent_dma_mask)
		card->dev->coherent_dma_mask = DMA_BIT_MASK(32);

	if (!card->dev->dma_mask)
		card->dev->dma_mask = &card->dev->coherent_dma_mask;

	if (pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream) {
		ret = ipq4019_pcm_preallocate_dma_buffer(pcm,
				SNDRV_PCM_STREAM_PLAYBACK);
		if (ret) {
			dev_err(card->dev,
				"Error allocating playback dma. ret: %d\n",
									ret);
			return -ENOMEM;
		}
		pback = 1;
	}

	if (pcm->streams[SNDRV_PCM_STREAM_CAPTURE].substream) {
		ret = ipq4019_pcm_preallocate_dma_buffer(pcm,
				SNDRV_PCM_STREAM_CAPTURE);
		if (ret) {
			dev_err(card->dev,
				"Error allocating capture dma buf. ret: %d\n",
									ret);
			if (pback)
				ipq4019_pcm_free_dma_buffer(pcm,
						SNDRV_PCM_STREAM_PLAYBACK);
			return -ENOMEM;
		}
	}

	return ret;
}

static struct snd_soc_platform_driver ipq4019_asoc_pcm_i2s_platform = {
	.ops		= &ipq4019_asoc_pcm_i2s_ops,
	.pcm_new	= ipq4019_asoc_pcm_i2s_new,
	.pcm_free	= ipq4019_asoc_pcm_i2s_free,
};

static const struct of_device_id ipq4019_pcm_i2s_id_table[] = {
	{ .compatible = "qca,ipq4019-pcm-i2s" },
	{ /* Sentinel */ },
};
MODULE_DEVICE_TABLE(of, ipq4019_pcm_i2s_id_table);

static int ipq4019_pcm_i2s_driver_probe(struct platform_device *pdev)
{
	int ret;

	ret = snd_soc_register_platform(&pdev->dev,
			&ipq4019_asoc_pcm_i2s_platform);
	if (ret)
		dev_err(&pdev->dev,
			"Failed to register i2s pcm device ret: %d\n", ret);
	return ret;
}

static int ipq4019_pcm_i2s_driver_remove(struct platform_device *pdev)
{
	snd_soc_unregister_platform(&pdev->dev);
	return 0;
}

static struct platform_driver ipq4019_pcm_i2s_driver = {
	.probe = ipq4019_pcm_i2s_driver_probe,
	.remove = ipq4019_pcm_i2s_driver_remove,
	.driver = {
		.name = "qca-pcm-i2s",
		.of_match_table = ipq4019_pcm_i2s_id_table,
	},
};

module_platform_driver(ipq4019_pcm_i2s_driver);

MODULE_ALIAS("platform:qca-pcm-i2s");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("IPQ4019 PCM I2S Platform Driver");
