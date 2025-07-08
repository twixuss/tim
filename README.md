# Twixuss' IMage format

Compress images by:
1. Computing mip map and deltas between it and original image.
3. Comressing deltas, which are usually small.
4. Compressing the mipmap again until 1x1.

Comparison with other formats on a 4k image:

Format|Size|Percentage|Graph
-|-|-|-
jpg|10 MiB|21%|`====`
qoi|20 MiB|41%|`========`
png|22 MiB|45%|`=========`
tim|22 MiB|47%|`=========`
tga|40 MiB|83%|`================`
bmp|48 MiB|100%|`====================`

Format|Enc. speed|Percentage|Graph
-|-|-|-
bmp|419 MiB/s|100%|`====================`
qoi|413 MiB/s|98%|`===================`
tga|240 MiB/s|57%|`===========`
jpg|196 MiB/s|46%|`=========`
png|127 MiB/s|30%|`======`
tim|99 MiB/s|23%|`====`

Format|Dec. speed|Percentage|Graph
-|-|-|-
bmp|343 MiB/s|100%|`====================`
tga|297 MiB/s|86%|`=================`
qoi|163 MiB/s|47%|`=========`
jpg|57 MiB/s|16%|`===`
tim|46 MiB/s|13%|`==`
png|17 MiB/s|5%|`=`
