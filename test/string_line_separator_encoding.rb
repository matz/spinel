# A line walk validates the separator encoding before splitting bytes.
def opaque(value)
  [value, 1][0]
end

def report
  p yield
rescue Encoding::CompatibilityError => error
  p error.message
end

report { "é".lines("\xA9".b) }
report { "é".lines("\xA9".b, chomp: true) }
report { "é".each_line("\xA9".b).next }
report { "é".each_line("\xA9".b) { |line| p line.bytes } }
report { opaque("é").lines("\xA9".b, chomp: true) }
report { "\xA9".b.lines("é", chomp: true) }
report { "".lines("\xA9".b, chomp: true) }
report { "é".lines("\n".b, chomp: true) }
