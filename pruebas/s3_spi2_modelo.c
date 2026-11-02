/* Extensiones S3 del banco compartido: mismo motor y servicios DMA absolutos. */
static void pruebas_s3(void)
{
    reset();
    assert(leer(0xf0) == 0x02101190u); /* SPI_DATE propio del S3. */
    /* HAL S3 despierta la primera ISR escribiendo INT_SET, no RAW como C3. */
    escribir(0x44, C3_SPI_DONE);
    assert(leer(0x3c) == C3_SPI_DONE && !nivel_irq && !leer(0x44));
    escribir(0x34, C3_SPI_DONE | C3_SPI_SEG_DONE);
    assert(nivel_irq && leer(0x40) == C3_SPI_DONE);
    escribir(0x44, C3_SPI_SEG_DONE | 0xffe00000u);
    assert(leer(0x3c) == (C3_SPI_DONE | C3_SPI_SEG_DONE));
    escribir(0x38, C3_SPI_DONE);
    assert(nivel_irq && leer(0x3c) == C3_SPI_SEG_DONE);
    escribir(0x38, C3_SPI_SEG_DONE);
    assert(!nivel_irq);
    /* Últimos 16 bytes DMA S3: descriptor y buffer fuera del rango C3. */
    for (unsigned d = 0; d < 2; d++) {
        reset();
        uint32_t descriptor = FIN_SRAM - 12, buffer = FIN_SRAM - 16;
        unsigned desplazamiento = descriptor - BASE_SRAM;
        uint8_t datos[4] = {0xa1, 0xb2, 0xc3, 0xd4};
        memcpy(ram + buffer - BASE_SRAM, datos, 4);
        stl_le_p(ram + desplazamiento, C3_DMA_OWNER | C3_DMA_EOF | (4u << 12) | 4);
        stl_le_p(ram + desplazamiento + 4, buffer);
        stl_le_p(ram + desplazamiento + 8, 0);
        arrancar(d, 4);
        canales[d][4].link = (descriptor & 0xfffff) | (1u << (d ? 21 : 22));
        assert(esp32c3_spi2_dma_copiar(&spi, !d, datos, 4) == 4);
        assert(canales[d][4].suc_eof_desc_addr == descriptor);
        assert(canales[d][4].int_state.raw == (d ? 0xbu : 3u));
        assert(!memcmp(ram + buffer - BASE_SRAM, datos, 4));
    }
}
