# spinel: gc-minor
m = "ab".match(/(?<word>ab)(?<empty>)(?<missing>x)?/)
strings = m.named_captures
symbols = m.named_captures(symbolize_names: true)
subset = m.deconstruct_keys([:word, :empty, :missing])
strings["word"] << "!"
symbols[:empty] << "?"
subset[:word] << "."
p [strings["word"], strings["empty"], strings["missing"]]
p [symbols[:word], symbols[:empty], symbols[:missing]]
p [subset[:word], subset[:empty], subset[:missing]]
p [m[:word], m[:empty]]
