# Inherit device configuration for shieldtablet.
$(call inherit-product, device/nvidia/shieldtablet/full_shieldtablet.mk)

# Inherit some common ZOMBI stuff.
$(call inherit-product, vendor/zombi/config/common.mk)

PRODUCT_NAME := zombi_shieldtablet
PRODUCT_DEVICE := shieldtablet
