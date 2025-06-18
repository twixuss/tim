# Twixuss' IMage format

Compress images by:
1. Computing mip map and deltas between it and original image.
3. Comressing deltas, which are usually small.
4. Compressing the mipmap again until 1x1.

Comparison with other formats on a 4k image:


Format|Size|Percentage|Graph
-|-|-|-
bmp|48.0 MiB|100%|`====================`
tga|40.2 MiB|83.8%|`================`
|***tim***|***22.9 MiB***|***47.9%***|`==========`
png|22.0 MiB|45.9%|`=========`
qoi|20.0 MiB|41.6%|`========`
jpg|10.1 MiB|21.1%|`====`

Format|Encode time|Encode speed|Graph
-|-|-|-
bmp|109.9 ms|436 MiB/s|`=========`
qoi|115.8 ms|414 MiB/s|`========`
tga|200.3 ms|239 MiB/s|`=====`
jpg|245.3 ms|195 MiB/s|`====`
png|377.5 ms|127 MiB/s|`===`
|***tim***|***945.2 ms***|***50.7 MiB/s***|`=`


Format|Decode time|Decode speed|Graph
-|-|-|-
bmp|145.8 ms|329 MiB/s|`==================`
tga|165.8 ms|289 MiB/s|`================`
qoi|288.8 ms|166 MiB/s|`=========`
|***tim***|***308.1 ms***|***155 MiB/s***|`========`
jpg|846.3 ms|56.7 MiB/s|`===`
png|2602 ms|18.4 MiB/s|`=`
