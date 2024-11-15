/*
 * Copyright (c) 2024 Arm Limited.
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
 * @file time_domains.cpp
 *
 * @brief Contains implemenations and details for the time domains per backend.
 */

#include "time_domains.hpp"
#include <vulkan/vulkan.h>
namespace wsi
{
#if VULKAN_WSI_LAYER_EXPERIMENTAL

VkResult swapchain_time_domains::calibrate(VkPresentStageFlagBitsEXT present_stage,
                                           swapchain_calibrated_time *calibrated_time)
{
   for (auto &domain : m_time_domains)
   {
      if ((domain->get_present_stages() & present_stage) != 0)
      {
         *calibrated_time = domain->calibrate();
         return VK_SUCCESS;
      }
   }

   return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult swapchain_time_domains::set_swapchain_time_domain_properties(
   VkSwapchainTimeDomainPropertiesEXT *pSwapchainTimeDomainProperties, uint64_t *pTimeDomainsCounter)
{
   if (pTimeDomainsCounter != nullptr)
   {
      if (pSwapchainTimeDomainProperties == nullptr)
      {
         *pTimeDomainsCounter = 1;
         return VK_SUCCESS;
      }
      pSwapchainTimeDomainProperties->timeDomainCount = 1;
      pSwapchainTimeDomainProperties->pTimeDomains[0] = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
      pSwapchainTimeDomainProperties->pTimeDomainIds = 0;

      return (*pTimeDomainsCounter < 1) ? VK_INCOMPLETE : VK_SUCCESS;
   }

   if (pSwapchainTimeDomainProperties != nullptr)
   {
      if (pSwapchainTimeDomainProperties->pTimeDomains == nullptr &&
          pSwapchainTimeDomainProperties->pTimeDomainIds == nullptr)
      {
         pSwapchainTimeDomainProperties->timeDomainCount = 1;
         return VK_SUCCESS;
      }
      if (pSwapchainTimeDomainProperties->pTimeDomains != nullptr &&
          pSwapchainTimeDomainProperties->pTimeDomainIds != nullptr)
      {
         pSwapchainTimeDomainProperties->timeDomainCount = 1;
         pSwapchainTimeDomainProperties->pTimeDomains[0] = VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
         pSwapchainTimeDomainProperties->pTimeDomainIds = 0;
         return VK_SUCCESS;
      }
   }

   return VK_SUCCESS;
}
#endif
}
