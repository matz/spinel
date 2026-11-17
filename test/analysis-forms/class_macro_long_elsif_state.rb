# A macro state written in an elsif chain longer than any fixed table: every
# branch is merged, so the last one (taken at run time) makes the state
# unknown and the macro reading it is left as written (refused).
module Consts
  def kind(k = nil)
    return @kind if k.nil?
    @kind = k
  end
  def constant(c)
    const_set(c, public_send(["calc", kind, c.to_s.downcase].join("_")))
  end
end
class Calc
  extend Consts
  def self.calc_a_size = 1
  def self.calc_b_size = 2
  kind :a
  if ARGV.size == 100
    kind :a
  elsif ARGV.size == 101
    kind :a
  elsif ARGV.size == 102
    kind :a
  elsif ARGV.size == 103
    kind :a
  elsif ARGV.size == 104
    kind :a
  elsif ARGV.size == 105
    kind :a
  elsif ARGV.size == 106
    kind :a
  elsif ARGV.size == 107
    kind :a
  elsif ARGV.size == 108
    kind :a
  elsif ARGV.size == 109
    kind :a
  elsif ARGV.size == 110
    kind :a
  elsif ARGV.size == 111
    kind :a
  elsif ARGV.size == 112
    kind :a
  elsif ARGV.size == 113
    kind :a
  elsif ARGV.size == 114
    kind :a
  elsif ARGV.size == 115
    kind :a
  elsif ARGV.size == 116
    kind :a
  elsif ARGV.size == 117
    kind :a
  elsif ARGV.size == 118
    kind :a
  elsif ARGV.size == 119
    kind :a
  elsif ARGV.size == 120
    kind :a
  elsif ARGV.size == 121
    kind :a
  elsif ARGV.size == 122
    kind :a
  elsif ARGV.size == 123
    kind :a
  elsif ARGV.size == 124
    kind :a
  elsif ARGV.size == 125
    kind :a
  elsif ARGV.size == 126
    kind :a
  elsif ARGV.size == 127
    kind :a
  elsif ARGV.size == 128
    kind :a
  elsif ARGV.size == 129
    kind :a
  elsif ARGV.size == 130
    kind :a
  elsif ARGV.size == 131
    kind :a
  elsif ARGV.size == 132
    kind :a
  elsif ARGV.size == 133
    kind :a
  elsif ARGV.size == 134
    kind :a
  elsif ARGV.size == 135
    kind :a
  elsif ARGV.size == 136
    kind :a
  elsif ARGV.size == 137
    kind :a
  elsif ARGV.size == 138
    kind :a
  elsif ARGV.size == 139
    kind :a
  elsif ARGV.size == 140
    kind :a
  elsif ARGV.size == 141
    kind :a
  elsif ARGV.size == 142
    kind :a
  elsif ARGV.size == 143
    kind :a
  elsif ARGV.size == 144
    kind :a
  elsif ARGV.size == 145
    kind :a
  elsif ARGV.size == 146
    kind :a
  elsif ARGV.size == 147
    kind :a
  elsif ARGV.size == 148
    kind :a
  elsif ARGV.size == 149
    kind :a
  elsif ARGV.size == 150
    kind :a
  elsif ARGV.size == 151
    kind :a
  elsif ARGV.size == 152
    kind :a
  elsif ARGV.size == 153
    kind :a
  elsif ARGV.size == 154
    kind :a
  elsif ARGV.size == 155
    kind :a
  elsif ARGV.size == 156
    kind :a
  elsif ARGV.size == 157
    kind :a
  elsif ARGV.size == 158
    kind :a
  elsif ARGV.size == 159
    kind :a
  elsif ARGV.size == 160
    kind :a
  elsif ARGV.size == 161
    kind :a
  elsif ARGV.size == 162
    kind :a
  elsif ARGV.size == 163
    kind :a
  elsif ARGV.empty?
    kind :b
  end
  constant :SIZE
end
p Calc::SIZE
