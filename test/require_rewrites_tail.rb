# spinel: gc-stress
# A `%w(...).each` loop of computed requires is unrolled into the lines it
# stands on (#9898, #10155). One that is the receiver of a call with
# arguments or a block, or has other code after it on its last line (a
# `; stmt`), is left a loop: the unrolling replaces the whole line and would
# drop that rest. The first two files are required statically, so the
# run-time loops find them loaded.
$LOAD_PATH.unshift __dir__
require_relative "require_tail_files/ta"
require_relative "require_tail_files/tb"
$seen = []

%w(ta tb).each do |v|
  require File.join("require_tail_files", v)
end.size; $after = 1
p $after

%w(ta tb).each do |v|
  require File.join("require_tail_files", v)
end.each_slice(1) { |a| $seen << a }
p $seen

# a trailing if/unless modifier does not stop the unrolling: with a true
# condition the files are loaded (only the run-time loop would not find them)
%w(td te).each { |v| require File.join("require_tail_files", v) } if $after == 1
p [TAIL_D, TAIL_E]
%w(tf).each do |v|
  require File.join("require_tail_files", v)
end unless $after == 0
p TAIL_F
# a lone `;` after the loop, and a modifier whose condition goes on to the
# next line, leave nothing to lose
%w(tg).each do |v|
  require File.join("require_tail_files", v)
end;
p TAIL_G
%w(th).each do |v|
  require File.join("require_tail_files", v)
end if
  $after == 1
p TAIL_H
puts "main"
