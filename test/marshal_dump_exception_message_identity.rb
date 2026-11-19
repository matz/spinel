# An exception's message and the original String share one Marshal link.

text = +"é\0msg"
error = RuntimeError.new(text)
p error.message.equal?(text)
p Marshal.dump([text, error]).unpack1("H*")
p Marshal.dump([error, text]).unpack1("H*")

binary = "\x80\0".b.freeze
error = ArgumentError.new(binary)
p error.message.equal?(binary)
p Marshal.dump([binary, error]).unpack1("H*")
