# Upstream provenance

- Source: https://github.com/78/esp-opus
- ESP Component Registry baseline: 1.0.5
- Registry commit: `5854a9f7de06ab3505b8fe6e8943db581c2cbe70`
- Local source snapshot commit: `f674cf08320b391f3cc952ebf26a08487b39f2a5`

Local changes:

- Build the upstream floating-point SILK and Opus sources instead of the
  Registry component's forced fixed-point configuration.
- Route float CELT inner products through `opus_p4_kernels`, which keeps a
  portable C reference and provides the replacement boundary for P4 PIE asm.
- Route the float CELT anti-collapse signed-noise fill through
  `opus_p4_kernels`. Its P4 assembly symbol is currently an ABI scaffold that
  tail-calls the portable C reference; asm/verify dispatch and permanent C
  fallback are implemented outside the upstream codec source.

The upstream API and codec source remain otherwise unchanged.
