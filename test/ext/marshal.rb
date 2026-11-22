# An ext-init library that uses Marshal compiles as C. The symbol interner
# of such a library is not static, and its forward declaration for Marshal
# said static: the C compiler stopped at "static declaration of
# 'sp_sym_intern' follows non-static declaration".
module ExtMarshal
  #: (Integer) -> Integer
  def self.round_trip(n)
    back = Marshal.load(Marshal.dump([n, :sym, "str"]))
    $stderr.puts back.inspect
    n + 1
  end
end
if __FILE__ == $0
  p ExtMarshal.round_trip(41)
end
