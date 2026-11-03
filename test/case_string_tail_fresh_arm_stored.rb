# A method whose value is a `case` over a String, stored where the caller
# keeps the String itself (`out << esc(ch)`): the caller takes the handle
# the method's last read published. The subject read published ch, and a
# `when` arm answering a new String left it there, so every arm stored ch
# and the else arm seemed to answer. Each arm is a tail now: a new String's
# arm clears the channel, a read's arm publishes its own String.
def esc(ch)
  case ch
  when "\"" then "\\\""
  when "\n" then "\\n"
  when "é" then "&eacute;"
  when "日" then "&#26085;"
  else ch
  end
end

def esc_ret(ch)
  return case ch
         when "\"" then "Q"
         else ch
         end
end

def esc_ret_paren(ch)
  return (case ch
          when "\"" then "Q"
          else ch
          end)
end

def esc_paren(ch)
  ((case ch
    when "\"" then "Q"
    else ch
    end))
end

def esc_subjectless(ch)
  case
  when ch == "\"" then "Q"
  else ch
  end
end

def esc_builtin(ch)
  case ch
  when "a" then ch.upcase + "!"
  else ch
  end
end

def esc_nested(ch)
  if ch.size == 1
    case ch
    when "\"" then "Q"
    else ch
    end
  else
    ch
  end
end

src = "a\"b\ncéd日"

out = []
src.each_char { |ch| out << esc(ch) }
puts out.join
p out

p src.chars.map { |c| esc(c) }

out = []
src.chars.each { |c| out << esc(c) }
puts out.join

out = []
out << esc("\"") << esc("é") << esc("z")
p out

out = []
src.each_char { |ch| out << esc_ret(ch) }
p out

out = []
src.each_char { |ch| out << esc_ret_paren(ch) << esc_paren(ch) }
p out

out = []
src.each_char { |ch| out << esc_subjectless(ch) }
p out

out = []
src.each_char { |ch| out << esc_builtin(ch) }
p out

out = []
src.each_char { |ch| out << esc_nested(ch) }
p out

# the else arm still answers the String it was handed, not a copy
s = +"x"
kept = []
kept << esc(s)
kept[0] << "!"
p s
p kept[0].equal?(s)

# so does the else arm of a case in parentheses
[:esc_ret_paren, :esc_paren].each do |m|
  s = +"y"
  kept = []
  kept << send(m, s)
  kept[0] << "?"
  p [s, kept[0].equal?(s)]
end
