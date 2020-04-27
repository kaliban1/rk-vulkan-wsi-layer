/*
 * Copyright (c) 2017-2019 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file swapchain_base.cpp
 *
 * @brief Contains the implementation for the swapchain.
 *
 * This file contains much of the swapchain implementation,
 * that is not specific to how images are created or presented.
 */

#include <array>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <unistd.h>
#include <vulkan/vulkan.h>

#include "swapchain_base.hpp"

#if VULKAN_WSI_DEBUG > 0
#define WSI_PRINT_ERROR(...) fprintf(stderr, ##__VA_ARGS__)
#else
#define WSI_PRINT_ERROR(...) (void)0
#endif

namespace wsi
{

/**
 * @brief Per swapchain thread function that handles page flipping.
 * This thread should be running for the lifetime of the swapchain.
 * The thread simply calls the implementation's present_image() method.
 * There are 3 main cases we cover here:
 *
 * 1. On the first present of the swapchain if the swapchain has
 *    an ancestor we must wait for it to finish presenting.
 * 2. The normal use case where we do page flipping, in this
 *    case change the currently PRESENTED image with the oldest
 *    PENDING image.
 * 3. If the enqueued image is marked as FREE it means the
 *    descendant of the swapchain has started presenting so we
 *    should release the image and continue.
 *
 * The function always waits on the page_flip_semaphore of the
 * swapchain. Once it passes that we must wait for the fence of the
 * oldest pending image to be signalled, this means that the gpu has
 * finished rendering to it and we can present it. From there on the
 * logic splits into the above 3 cases and if an image has been
 * presented then the old one is marked as FREE and the free_image
 * semaphore of the swapchain will be posted.
 **/
void *page_flip_thread(void *ptr)
{
   auto *sc = static_cast<swapchain_base *>(ptr);
   wsi::swapchain_image *sc_images = sc->m_swapchain_images;
   VkResult vk_res = VK_SUCCESS;
   uint64_t timeout = UINT64_MAX;

   while (true)
   {
      /* Check if this thread needs to be terminated. */
      if (!sc->m_page_flip_thread_run)
      {
         break;
      }
      /* Waiting for the page_flip_semaphore which will be signalled once there is an
       * image to display.*/
      if ((vk_res = sc->m_page_flip_semaphore.wait(0)) == VK_NOT_READY)
      {
         /* Image is not ready yet. */
         continue;
      }
      assert(vk_res == VK_SUCCESS);

      /* We want to present the oldest queued for present image from our present queue,
       * which we can find at the sc->pending_buffer_pool.head index. */
      uint32_t pending_index = sc->m_pending_buffer_pool.ring[sc->m_pending_buffer_pool.head];
      sc->m_pending_buffer_pool.head = (sc->m_pending_buffer_pool.head + 1) % sc->m_pending_buffer_pool.size;

      /* We wait for the fence of the oldest pending image to be signalled. */
      vk_res = sc->m_device_data.disp.WaitForFences(sc->m_device, 1, &sc_images[pending_index].present_fence, VK_TRUE,
                                                    timeout);
      if (vk_res != VK_SUCCESS)
      {
         sc->m_is_valid = false;
         sc->m_free_image_semaphore.post();
         continue;
      }

      /* If the descendant has started presenting the queue_present operation has marked the image
       * as FREE so we simply release it and continue. */
      if (sc_images[pending_index].status == swapchain_image::FREE)
      {
         sc->destroy_image(sc_images[pending_index]);
         sc->m_free_image_semaphore.post();
         continue;
      }

      /* First present of the swapchain. If it has an ancestor, wait until all the pending buffers
       * from the ancestor have finished page flipping before we set mode. */
      if (sc->m_first_present)
      {
         if (sc->m_ancestor != VK_NULL_HANDLE)
         {
            auto *ancestor = reinterpret_cast<swapchain_base *>(sc->m_ancestor);
            ancestor->wait_for_pending_buffers();
         }

         sem_post(&sc->m_start_present_semaphore);

         sc->present_image(pending_index);

         sc->m_first_present = false;
      }
      /* The swapchain has already started presenting. */
      else
      {
         sc->present_image(pending_index);
      }
   }
   return nullptr;
}

void swapchain_base::unpresent_image(uint32_t presented_index)
{
   m_swapchain_images[presented_index].status = swapchain_image::FREE;

   if (m_descendant != VK_NULL_HANDLE)
   {
      destroy_image(m_swapchain_images[presented_index]);
   }

   m_free_image_semaphore.post();
}

swapchain_base::swapchain_base(layer::device_private_data &dev_data, const VkAllocationCallbacks *allocator)
   : m_device_data(dev_data)
   , m_thread_sem_defined(false)
   , m_first_present(true)
   , m_pending_buffer_pool{ nullptr, 0, 0, 0 }
   , m_num_swapchain_images(0)
   , m_swapchain_images(nullptr)
   , m_alloc_callbacks(allocator)
   , m_surface(VK_NULL_HANDLE)
   , m_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
   , m_descendant(VK_NULL_HANDLE)
   , m_ancestor(VK_NULL_HANDLE)
   , m_device(VK_NULL_HANDLE)
   , m_queue(VK_NULL_HANDLE)
   , m_page_flip_thread_run(true)
{
}

VkResult swapchain_base::init(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info)
{
   assert(device != VK_NULL_HANDLE);
   assert(swapchain_create_info != nullptr);
   assert(swapchain_create_info->surface != VK_NULL_HANDLE);

   int res;
   VkResult result;

   m_device = device;
   m_surface = swapchain_create_info->surface;

   /* Check presentMode has a compatible value with swapchain - everything else should be taken care at image creation.*/
   static const std::array<VkPresentModeKHR, 2> present_modes = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR };
   bool present_mode_found = false;
   for (uint32_t i = 0; i < present_modes.size() && !present_mode_found; i++)
   {
      if (swapchain_create_info->presentMode == present_modes[i])
      {
         present_mode_found = true;
      }
   }

   if (!present_mode_found)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   m_num_swapchain_images = swapchain_create_info->minImageCount;

   size_t images_alloc_size = sizeof(swapchain_image) * m_num_swapchain_images;
   if (m_alloc_callbacks != nullptr)
   {
      m_swapchain_images = static_cast<swapchain_image *>(m_alloc_callbacks->pfnAllocation(
         m_alloc_callbacks->pUserData, images_alloc_size, alignof(swapchain_image), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
   }
   else
   {
      m_swapchain_images = static_cast<swapchain_image *>(malloc(images_alloc_size));
   }

   if (m_swapchain_images == nullptr)
   {
      m_num_swapchain_images = 0;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   /* We have allocated images, we can call the platform init function if something needs to be done. */
   result = init_platform(device, swapchain_create_info);
   if (result != VK_SUCCESS)
   {
      return result;
   }

   for (uint32_t i = 0; i < m_num_swapchain_images; ++i)
   {
      /* Init image to invalid values. */
      m_swapchain_images[i].image = VK_NULL_HANDLE;
      m_swapchain_images[i].present_fence = VK_NULL_HANDLE;
      m_swapchain_images[i].status = swapchain_image::INVALID;
      m_swapchain_images[i].data = nullptr;
   }

   /* Initialize ring buffer. */
   if (m_alloc_callbacks != nullptr)
   {
      m_pending_buffer_pool.ring = static_cast<uint32_t *>(
         m_alloc_callbacks->pfnAllocation(m_alloc_callbacks->pUserData, sizeof(uint32_t) * m_num_swapchain_images,
                                          alignof(uint32_t), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT));
   }
   else
   {
      m_pending_buffer_pool.ring = static_cast<uint32_t *>(malloc(sizeof(uint32_t) * m_num_swapchain_images));
   }

   if (m_pending_buffer_pool.ring == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   m_pending_buffer_pool.head = 0;
   m_pending_buffer_pool.tail = 0;
   m_pending_buffer_pool.size = m_num_swapchain_images;

   VkImageCreateInfo image_create_info = {};
   image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
   image_create_info.pNext = nullptr;
   image_create_info.imageType = VK_IMAGE_TYPE_2D;
   image_create_info.format = swapchain_create_info->imageFormat;
   image_create_info.extent = { swapchain_create_info->imageExtent.width, swapchain_create_info->imageExtent.height, 1 };
   image_create_info.mipLevels = 1;
   image_create_info.arrayLayers = swapchain_create_info->imageArrayLayers;
   image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
   image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
   image_create_info.usage = swapchain_create_info->imageUsage;
   image_create_info.flags = 0;
   image_create_info.sharingMode = swapchain_create_info->imageSharingMode;
   image_create_info.queueFamilyIndexCount = swapchain_create_info->queueFamilyIndexCount;
   image_create_info.pQueueFamilyIndices = swapchain_create_info->pQueueFamilyIndices;
   image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

   result = m_free_image_semaphore.init(m_num_swapchain_images);
   if (result != VK_SUCCESS)
   {
      assert(result == VK_ERROR_OUT_OF_HOST_MEMORY);
      return result;
   }

   for (unsigned i = 0; i < m_num_swapchain_images; i++)
   {
      result = create_image(image_create_info, m_swapchain_images[i]);
      if (result != VK_SUCCESS)
      {
         return result;
      }
   }

   m_device_data.disp.GetDeviceQueue(m_device, 0, 0, &m_queue);
   result = m_device_data.SetDeviceLoaderData(m_device, m_queue);
   if (VK_SUCCESS != result)
   {
      return result;
   }

   /* Setup semaphore for signaling pageflip thread */
   result = m_page_flip_semaphore.init(0);
   if (result != VK_SUCCESS)
   {
      return result;
   }

   /* Only programming error can cause this to fail. */
   assert(res == 0);
   if (res != 0)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   res = sem_init(&m_start_present_semaphore, 0, 0);
   /* Only programming error can cause this to fail. */
   assert(res == 0);
   if (res != 0)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   m_thread_sem_defined = true;

   /* Launch page flipping thread */
   m_page_flip_thread = std::thread(page_flip_thread, static_cast<void *>(this));

   /* Release the swapchain images of the old swapchain in order
    * to free up memory for new swapchain. This is necessary especially
    * on platform with limited display memory size.
    *
    * NB: This must be done last in initialization, when the rest of
    * the swapchain is valid.
    */
   if (swapchain_create_info->oldSwapchain != VK_NULL_HANDLE)
   {
      /* Set ancestor. */
      m_ancestor = swapchain_create_info->oldSwapchain;

      auto *ancestor = reinterpret_cast<swapchain_base *>(m_ancestor);
      ancestor->deprecate(reinterpret_cast<VkSwapchainKHR>(this));
   }

   m_is_valid = true;

   return VK_SUCCESS;
}

void swapchain_base::teardown()
{
   /* This method will block until all resources associated with this swapchain
    * are released. Images in the ACQUIRED or FREE state can be freed
    * immediately. For images in the PRESENTED state, we will block until the
    * presentation engine is finished with them. */

   int res;
   bool descendent_started_presenting = false;

   if (m_descendant != VK_NULL_HANDLE)
   {
      auto *desc = reinterpret_cast<swapchain_base *>(m_descendant);
      for (uint32_t i = 0; i < desc->m_num_swapchain_images; ++i)
      {
         if (desc->m_swapchain_images[i].status == swapchain_image::PRESENTED ||
             desc->m_swapchain_images[i].status == swapchain_image::PENDING)
         {
            /* Here we wait for the start_present_semaphore, once this semaphore is up,
             * the descendant has finished waiting, we don't want to delete vkImages and vkFences
             * and semaphores before the waiting is done. */
            sem_wait(&desc->m_start_present_semaphore);

            descendent_started_presenting = true;
            break;
         }
      }
   }

   /* If descendant started presenting, there is no pending buffer in the swapchain. */
   if (descendent_started_presenting == false)
   {
      wait_for_pending_buffers();
   }

   /* Make sure the vkFences are done signaling. */
   m_device_data.disp.QueueWaitIdle(m_queue);

   /* We are safe to destroy everything. */

   if (m_thread_sem_defined)
   {
      /* Tell flip thread to end. */
      m_page_flip_thread_run = false;

      if (m_page_flip_thread.joinable())
      {
         m_page_flip_thread.join();
      }
      else
      {
         WSI_PRINT_ERROR("m_page_flip_thread is not joinable");
      }

      res = sem_destroy(&m_start_present_semaphore);
      if (res != 0)
      {
         WSI_PRINT_ERROR("sem_destroy failed for start_present_semaphore with %d\n", errno);
      }
   }

   if (m_descendant != VK_NULL_HANDLE)
   {
      auto *sc = reinterpret_cast<swapchain_base *>(m_descendant);
      sc->clear_ancestor();
   }

   if (m_ancestor != VK_NULL_HANDLE)
   {
      auto *sc = reinterpret_cast<swapchain_base *>(m_ancestor);
      sc->clear_descendant();
   }

   /* Release the images array. */
   if (m_swapchain_images != nullptr)
   {

      for (uint32_t i = 0; i < m_num_swapchain_images; ++i)
      {
         /* Call implementation specific release */
         destroy_image(m_swapchain_images[i]);
      }

      if (m_alloc_callbacks != nullptr)
      {
         m_alloc_callbacks->pfnFree(m_alloc_callbacks->pUserData, m_swapchain_images);
      }
      else
      {
         free(m_swapchain_images);
      }
   }

   /* Free ring buffer. */
   if (m_pending_buffer_pool.ring != nullptr)
   {
      if (m_alloc_callbacks != nullptr)
      {
         m_alloc_callbacks->pfnFree(m_alloc_callbacks->pUserData, m_pending_buffer_pool.ring);
      }
      else
      {
         free(m_pending_buffer_pool.ring);
      }
   }
}

VkResult swapchain_base::acquire_next_image(uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *image_index)
{
   VkResult retval = wait_for_free_buffer(timeout);
   if (retval != VK_SUCCESS)
   {
      return retval;
   }

   if (!m_is_valid)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   uint32_t i;
   for (i = 0; i < m_num_swapchain_images; ++i)
   {
      if (m_swapchain_images[i].status == swapchain_image::FREE)
      {
         m_swapchain_images[i].status = swapchain_image::ACQUIRED;
         *image_index = i;
         break;
      }
   }

   assert(i < m_num_swapchain_images);

   if (VK_NULL_HANDLE != semaphore || VK_NULL_HANDLE != fence)
   {
      VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };

      if (VK_NULL_HANDLE != semaphore)
      {
         submit.signalSemaphoreCount = 1;
         submit.pSignalSemaphores = &semaphore;
      }

      submit.commandBufferCount = 0;
      submit.pCommandBuffers = nullptr;
      retval = m_device_data.disp.QueueSubmit(m_queue, 1, &submit, fence);
      assert(retval == VK_SUCCESS);
   }

   return retval;
}

VkResult swapchain_base::get_swapchain_images(uint32_t *swapchain_image_count, VkImage *swapchain_images)
{
   if (swapchain_images == nullptr)
   {
      /* Return the number of swapchain images. */
      *swapchain_image_count = m_num_swapchain_images;

      return VK_SUCCESS;
   }
   else
   {
      assert(m_num_swapchain_images > 0);
      assert(*swapchain_image_count > 0);

      /* Populate array, write actual number of images returned. */
      uint32_t current_image = 0;

      do
      {
         swapchain_images[current_image] = m_swapchain_images[current_image].image;

         current_image++;

         if (current_image == m_num_swapchain_images)
         {
            *swapchain_image_count = current_image;

            return VK_SUCCESS;
         }

      } while (current_image < *swapchain_image_count);

      /* If swapchain_image_count is smaller than the number of presentable images
       * in the swapchain, VK_INCOMPLETE must be returned instead of VK_SUCCESS. */
      *swapchain_image_count = current_image;

      return VK_INCOMPLETE;
   }
}

VkResult swapchain_base::queue_present(VkQueue queue, const VkPresentInfoKHR *present_info, const uint32_t image_index)
{
   VkResult result;
   bool descendent_started_presenting = false;

   if (m_descendant != VK_NULL_HANDLE)
   {
      auto *desc = reinterpret_cast<swapchain_base *>(m_descendant);
      for (uint32_t i = 0; i < desc->m_num_swapchain_images; ++i)
      {
         if (desc->m_swapchain_images[i].status == swapchain_image::PRESENTED ||
             desc->m_swapchain_images[i].status == swapchain_image::PENDING)
         {
            descendent_started_presenting = true;
            break;
         }
      }
   }

   /* When the semaphore that comes in is signalled, we know that all work is done. So, we do not
    * want to block any future Vulkan queue work on it. So, we pass in BOTTOM_OF_PIPE bit as the
    * wait flag.
    */
   VkPipelineStageFlags pipeline_stage_flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

   VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               NULL,
                               present_info->waitSemaphoreCount,
                               present_info->pWaitSemaphores,
                               &pipeline_stage_flags,
                               0,
                               NULL,
                               0,
                               NULL };

   assert(m_swapchain_images[image_index].status == swapchain_image::ACQUIRED);
   result = m_device_data.disp.ResetFences(m_device, 1, &m_swapchain_images[image_index].present_fence);
   if (result != VK_SUCCESS)
   {
      return result;
   }

   result = m_device_data.disp.QueueSubmit(queue, 1, &submit_info, m_swapchain_images[image_index].present_fence);
   if (result != VK_SUCCESS)
   {
      return result;
   }

   /* If the descendant has started presenting, we should release the image
    * however we do not want to block inside the main thread so we mark it
    * as free and let the page flip thread take care of it. */
   if (descendent_started_presenting)
   {
      m_swapchain_images[image_index].status = swapchain_image::FREE;

      m_pending_buffer_pool.ring[m_pending_buffer_pool.tail] = image_index;
      m_pending_buffer_pool.tail = (m_pending_buffer_pool.tail + 1) % m_pending_buffer_pool.size;

      m_page_flip_semaphore.post();

      return VK_ERROR_OUT_OF_DATE_KHR;
   }

   m_swapchain_images[image_index].status = swapchain_image::PENDING;

   m_pending_buffer_pool.ring[m_pending_buffer_pool.tail] = image_index;
   m_pending_buffer_pool.tail = (m_pending_buffer_pool.tail + 1) % m_pending_buffer_pool.size;

   m_page_flip_semaphore.post();
   return VK_SUCCESS;
}

void swapchain_base::deprecate(VkSwapchainKHR descendant)
{
   for (unsigned i = 0; i < m_num_swapchain_images; i++)
   {
      if (m_swapchain_images[i].status == swapchain_image::FREE)
      {
         destroy_image(m_swapchain_images[i]);
      }
   }

   /* Set its descendant. */
   m_descendant = descendant;
}

void swapchain_base::wait_for_pending_buffers()
{
   int num_acquired_images = 0;
   int wait;

   for (uint32_t i = 0; i < m_num_swapchain_images; ++i)
   {
      if (m_swapchain_images[i].status == swapchain_image::ACQUIRED)
      {
         ++num_acquired_images;
      }
   }

   /* Once all the pending buffers are flipped, the swapchain should have images
    * in ACQUIRED (application fails to queue them back for presentation), FREE
    * and one and only one in PRESENTED. */
   wait = m_num_swapchain_images - num_acquired_images - 1;

   while (wait > 0)
   {
      /* Take down one free image semaphore. */
      wait_for_free_buffer(UINT64_MAX);
      --wait;
   }
}

void swapchain_base::clear_ancestor()
{
   m_ancestor = VK_NULL_HANDLE;
}

void swapchain_base::clear_descendant()
{
   m_descendant = VK_NULL_HANDLE;
}

VkResult swapchain_base::wait_for_free_buffer(uint64_t timeout)
{
   VkResult retval;
   /* first see if a buffer is already marked as free */
   retval = m_free_image_semaphore.wait(0);
   if (retval == VK_NOT_READY)
   {
      /* if not, we still have work to do even if timeout==0 -
       * the swapchain implementation may be able to get a buffer without
       * waiting */

      retval = get_free_buffer(&timeout);
      if (retval == VK_SUCCESS)
      {
         /* the sub-implementation has done it's thing, so re-check the
          * semaphore */
         retval = m_free_image_semaphore.wait(timeout);
      }
   }

   return retval;
}

#undef WSI_PRINT_ERROR

} /* namespace wsi */
