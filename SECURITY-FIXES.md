# RyzenAdj — güvenlik ve stabilite geçişi

Bu ağaç, upstream RyzenAdj kaynağı üzerinde yapılan bir güvenlik/stabilite denetiminin
sonucudur. Aşağıdaki her madde kaynak kodda doğrulanmış bir kusurdur; davranış değişikliği
gerektirenler yorum satırlarıyla ilgili dosyada da işaretlenmiştir.

Doğrulama: ağaç `-Wall -Wextra -Wformat=2 -Wimplicit-fallthrough=5` ile **uyarısız** derleniyor;
üretilen ikili Full RELRO + BIND_NOW + NX + PIE taşıyor.

---

## 1. Yanlış SMU mesajı gönderiliyordu — `lib/api.c`, `set_stapm_time()`

Renoir / Lucienne / Cezanne / Vangogh / Rembrandt / Mendocino / Phoenix / Hawk Point /
Krackan / Strix Point / Strix Halo ailelerinde `break` eksikti. Sonuç: doğru mesaj `0x18`
gönderildikten sonra akış bir sonraki `case`'e düşüyor ve **`0x4e` (Dragon Range / Fire Range
mesaj kimliği) da aynı MP1 mailbox'ına gönderiliyordu**. `err` de ikinci çağrının sonucuyla
eziliyor, yani dönüş değeri gerçek sonucu yansıtmıyordu.

`-Wimplicit-fallthrough=5` ile yakalandı. İki `break` eklendi.

## 2. Sınırsız busy-wait → root'ta kilitlenme — `lib/nb_smu_ops.c`

`smu_service_req()` ve `smu_service_test()` içinde `while (response == 0x0)` döngüsünün
çıkış koşulu yoktu. SMU yanıt vermezse (takılmış SMU, başarısız firmware handshake, başka
bir aracın işlemi sürerken) süreç root olarak sonsuza dek dönüyor, bir çekirdeği %100'de
tutuyor ve SMN/PCI config yolunu sürekli dövüyordu; yalnızca `SIGKILL` ile sonlanıyordu.

Artık `SMU_RESP_TIMEOUT_MS` (1000 ms) sınırlı, ilk 128 spin'den sonra 50 µs uykuya geçen bir
bekleme var. `ADJ_ERR_SMU_TIMEOUT` **zaten tanımlıydı ama hiçbir yerde üretilmiyordu**; şimdi
`_do_adjust`, `_do_adjust_psmu`, `_return_translated_smu_error` ve `main.c` üzerinden
kullanıcıya kadar yayılıyor.

## 3. Ölü hata kontrolü — `lib/linux/osdep_linux_smu_kernel_module.c`

`get_pm_table_size()` `uint32_t` döndürüp hata için `-1` kullanıyordu; çağıran taraf sonucu
`size_t` alana yazdıktan **sonra** `-1` ile karşılaştırıyordu. LP64'te
`(size_t)0xFFFFFFFF != (size_t)-1`, dolayısıyla hata dalı hiç çalışmıyor ve başarısız okuma
4 GiB'lik bir "PM tablo boyutu" üretiyordu. `-Wsign-compare` ile doğrulandı.

İmzalı out-parametreye çevrildi, `RYZENADJ_MAX_TABLE_SIZE` üst sınırı eklendi.

## 4. PM tablosu okumaları sınırsızdı — `lib/api.c`, `_read_float_value()`

152 getter bu makro üzerinden `table_values[OFFSET / 4]` okuyordu. Offsetler tablo
*sürümünden* geliyor, `table_size` ise farklı bir kaynaktan (ryzen_smu modülünün bildirdiği
değer, ya da bilinmeyen sürümler için 0x1000 fallback). İkisi uyuşmadığında — özellikle kmod
değeri SMU'nun ima ettiğinden küçükse — heap buffer sınırı aşılıyordu.

Her erişim artık `table_size`'a karşı sınırlanıyor, aşımda `NAN` dönüyor.

## 5. `/dev/mem` eşlemesi — `lib/linux/osdep_linux_mem.c`

- `copy_pm_table_mem()` sabit 0x1000'lik pencereden `size` bayt kopyalıyordu; tablo bir
  sayfadan büyükse mapping dışına taşıyordu. Artık eşlenen uzunluğa karşı kontrol ediliyor.
- `compare_pm_table_mem()` `phy_map`'i `MAP_FAILED` kontrolü olmadan dereference ediyordu
  (`(void *)-1` → segfault).
- `if (dev_mem_fd > 0)` — fd 0 geçerli bir tanıtıcıdır (stdin kapalıysa). `>= 0` yapıldı.
- `mmap` offset'i `(long)physAddr` olarak hizalanmadan veriliyordu; `mmap` sayfa hizalı offset
  ister, hizasız tablo adresi `EINVAL` ile başarısız oluyordu. Artık sayfa hizasına
  yuvarlanıyor, tablo başlangıcı eşleme içindeki offset olarak tutuluyor.
- Eşleme boyutu artık gerçek tablo boyutuna göre; `init_mem_obj()` imzası `size` alacak
  şekilde genişletildi (dahili API, `ryzenadj.h` değişmedi).
- Tekrarlı `init_table()` çağrılarında önceki eşleme sızdırılıyordu; artık remap öncesi
  `munmap` yapılıyor.
- `phy_map` ve `is_smu` `static` yapıldı (paylaşımlı kütüphaneden dışa sızan semboller).
- `O_CLOEXEC` eklendi.

## 6. Bellek hataları

- `init_table()`: `calloc()` dönüşü kontrol edilmiyordu. OOM'da hem `table_values[0]` NULL
  deref ediliyor, hem de `init_table()` ↔ `refresh_table()` arasında **sonsuz özyineleme**
  (stack overflow) oluşuyordu. Kontrol + `table_init_busy` re-entrancy guard eklendi.
- `init_table()` iki kez çağrıldığında eski `table_values` sızdırılıyordu.
- `init_ryzenadj()`: `os_access` başarısız olduğunda `ry` sızdırılıyordu.
- `free_os_access_obj_kmod(NULL)` NULL deref ediyordu (mem sürümünde koruma vardı, kmod'da yoktu).
- `main.c / show_table_dump()`: iki `malloc()` dönüşü kontrolsüzdü; `get_table_values()`
  NULL dönebiliyordu.
- Genel: `get_cpu_family` / `get_bios_if_ver` / `get_table_values` / `refresh_table` artık
  NULL handle'a `cleanup_ryzenadj()` gibi toleranslı.

## 7. Girdi doğrulama — `argparse.c`

`strtoul()` negatifleri sessizce sarıyordu. Doğrulanmış davranış (öncesi → sonrası):

| Girdi | Eski | Yeni |
|---|---|---|
| `--fast-limit=-5` | kabul, **4294967291 mW** SMU'ya gidiyor | reddedildi |
| `--fast-limit=-1` | kabul, `0xFFFFFFFF` = "ayarlanmadı" sentinel'i ile çakışıyor | reddedildi |
| `--fast-limit=4294967296` | kabul, sessizce **0**'a kırpılıyor | reddedildi |
| `--fast-limit=25000` | kabul | kabul (değişmedi) |

Ayrıca: boş değer reddi, `ARGPARSE_OPT_INTEGER` için `INT_MIN`/`INT_MAX` aralık kontrolü,
ve `strerror_r()`'ın GNU/XSI varyant karışıklığı düzeltildi (bazı derlemelerde hata mesajı
boş basılıyordu).

## 8. Derleme sıkılaştırma — `CMakeLists.txt`

Root çalışan bir ikili için hiçbir mitigasyon yoktu. Eklendi:

`-fstack-protector-strong`, `-fstack-clash-protection`, `-fcf-protection=full`,
`_FORTIFY_SOURCE=2` (Debug hariç), `-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack`, PIE,
`-fno-strict-aliasing` (bu kod tabanı MMIO/tablo buffer'ları üzerinde type-punning yapıyor;
LTO varsayılan olarak açık, bu gerçek bir miscompile riski).

**Distro notu:** hepsi `add_compile_options()` / `add_link_options()` ile **eklemeli**;
`CMAKE_C_FLAGS`'a atanmadıkları için portage'ın `make.conf` bayraklarını ezmiyorlar ve
ortamdan gelen `CFLAGS` bunları sessizce silemiyor. `CMAKE_BUILD_TYPE` boşsa `Release`'e
düşülüyor (LTO zaten koşulsuz açıktı, `-O` olmadan hem anlamsız hem `_FORTIFY_SOURCE`
etkisiz kalıyordu). Minimum CMake sürümü 3.13'e çıkarıldı (`add_link_options` gereksinimi).

Sıkılaştırmayı kapatmak için: `-DRYZENADJ_HARDENING=OFF`.

## 9. Diğer düzeltmeler

- `lib/cpuid.c`: `*(uint32_t *)&vendor[0] = regs[1]` biçimi strict aliasing ihlali (LTO ile
  miscompile riski) — `memcpy`'ye çevrildi, buffer NUL sonlandırıldı.
- `lib/cpuid.c`: Zen5/Zen6 (`0x1A`) case'inde `break` eksikti; desteklenmeyen bir modelde
  hem "unsupported model" hem de sahte "Unsupported family: 1Ah" basılıyordu.
- `main.c`: `FAM_MENDOCINO` `family_name()` tablosunda yoktu, "Unknown" olarak görünüyordu.
- `main.c`: `printf("...%04X...", index * 4, ...)` — `index` `size_t`, format `unsigned int`
  bekliyor; LP64'te varargs uyumsuzluğu (tanımsız davranış). `%04zX` / `PRIX32` yapıldı.
  `%9.3lf`'e `float` besleyen çağrılar da açıkça `double`'a çevrildi.
- `main.c`: `-1` sentinel'i `ARG_UNSET` (`UINT32_MAX`) olarak açıkça yazıldı.
- `main.c`: `--info` / `--dump-table` istenmiş ama tablo init'i başarısız olduysa artık
  bayraklar temizleniyor; ayarlamalar yine uygulanıyor ama olmayan tabloyu okumaya çalışan
  kod yolu çalıştırılmıyor.
- `main.c`: root değilken önden uyarı (hard fail değil — CAP_SYS_RAWIO veya izin verilmiş
  ryzen_smu sysfs ile çalışan kurulumlar bozulmasın diye).
- ryzen_smu backend: kısa `read()`/`write()` işlenmiyordu; sysfs binary attribute'ları tek
  seferde tamamlanmayabilir, bu durumda PM tablosunun kuyruğu bayat/sıfır veriyle doluyor ve
  telemetri olarak raporlanıyordu. `read_full()` / `write_full()` (EINTR-safe) eklendi.
  `lseek()` dönüşleri de kontrol ediliyor. Dosya tanıtıcıları `O_CLOEXEC`.
- `lib/win32/osdep_win32.cpp`: `free_os_access_obj()` koşulsuz olarak
  `gfpUnmapPhysicalMemory(handle, *pdwLinAddr)` çağırıyordu. `--info`/`--dump-table`
  kullanılmayan **her** çalıştırmada `init_mem_obj()` hiç çalışmadığı için bu, NULL fonksiyon
  pointer'ı üzerinden çağrı + NULL deref demekti. Ayrıca eşlemenin ilk dword'ünü (`*pdwLinAddr`)
  lineer adres yerine parametre olarak geçiyordu. `copy_pm_table`/`compare_pm_table` de
  sınırsızdı.
- C23 `[[maybe_unused]]` sözdizimi taşınabilir `RA_UNUSED` makrosuyla değiştirildi; C11
  taban çizgisi artık `CMakeLists.txt`'te sabit.

---

## Bilerek yapılmayanlar

- **SMU mesaj kimlikleri ve PM tablo offsetleri doğrulanmadı.** Bunlar tersine mühendislikle
  elde edilmiş, donanıma özgü sabitler; kaynak koddan doğrulanamaz. Yalnızca `set_stapm_time`
  düzeltildi, çünkü oradaki sorun kimliğin *değeri* değil, iki kimliğin birden gönderilmesiydi.
- **`libryzenadj` kurulmuyor.** `BUILD_SHARED_LIBS` varsayılan `ON` olduğu halde `install()`
  yalnızca çalıştırılabilir dosyayı kapsıyor. Upstream davranışı; kasıtlı olabileceği için
  değiştirilmedi.
- **`smu_service_test()` "PCI Bus is not writeable" mesajı** MP1 ve PSMU için iki kez basılıyor.
  Çıktıyı ayrıştıran script'leri bozmamak için dokunulmadı.
