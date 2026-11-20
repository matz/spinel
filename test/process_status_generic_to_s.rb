# A Process::Status reached through a generic path reads as CRuby's to_s and
# inspect: an interpolation, String(), Array#join and the inspect of an Array
# holding one. The generic to_s of the runtime had no arm for the status and
# answered "" there, and an interpolation of one gave the object's address.
# (the pid differs per run, so it is masked)
def masked(s) = s.gsub(/pid \d+/, "pid N")

system("exit 3")
st = $?
puts masked("#{st}")
puts masked(String(st))
puts masked([st].join)
puts masked([st, "then"].join(", "))
puts masked([st].inspect)
puts masked("#{[st]}")
puts masked(st.to_s)
puts masked(st.inspect)

system("sh", "-c", "kill -TERM $$")
puts masked("#{$?}")
puts masked([$?].join)
