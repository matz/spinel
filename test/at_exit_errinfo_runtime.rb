# An exception the runtime raises gets its object for the hooks with what a
# rescue would read on it: a message, and here the key and receiver of a KeyError.
at_exit do
  puts "#{$!.class}: #{$!.message}"
  puts "key #{$!.key.inspect}" if $!.is_a?(KeyError)
  puts "receiver #{$!.receiver.inspect}" if $!.is_a?(KeyError)
end
h = {a: 1}
h.fetch(:b)
