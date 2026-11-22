# A rooted local keeps a one-argument fetch receiver alive. Index evaluation
# that can replace that local, a temporary receiver, defaults and blocks must
# retain the receiver's temporary root.
# spinel: decisions

def fetch_current(words, pc)
  words.fetch(pc)
end

def fetch_next(words, pc)
  words.fetch(pc + 1)
end

def fetch_literal(words)
  words.fetch(0)
end

words = [11, 22, 33]
puts fetch_current(words, 0)
puts fetch_current(words, -1)
puts fetch_next(words, 0)
puts fetch_next(words, -2)
puts fetch_literal(words)
begin
  fetch_next(words, 20)
rescue IndexError
  puts "bounds"
end
puts words.fetch(20, 77)
puts words.fetch(20) { |i| i + 1 }

# Replacing the receiver slot while the index runs must retain the old array.
20.times do
  held = (1..40).map { |i| i * 10 }
  answer = held.fetch(begin
    held = [99]
    GC.start
    (1..80).map { |i| [i, i.to_s] }
    5
  end)
  raise "lost receiver" unless answer == 60 && held[0] == 99
end
puts "replaced"

def temporary_words
  (1..40).map { |i| i * 10 }
end

def allocating_index
  GC.start
  (1..80).map { |i| [i, i.to_s] }
  5
end
puts temporary_words.fetch(allocating_index)

# A closure may overwrite the slot indirectly.
held = (1..40).map { |i| i * 10 }
replace = -> { held = [99]; GC.start; 5 }
puts held.fetch(replace.call)
puts held[0]

# Array#[] produces a nilable integer. A plain local containing that
# value is safe to use as an index even when conversion raises.
def fetch_indexed(words, indices, pc)
  index = indices[pc]
  words.fetch(index)
end

puts fetch_indexed(words, [-1], 0)
begin
  fetch_indexed(words, [0], 1)
rescue TypeError => e
  puts e.message
end
