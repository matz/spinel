# Proc#parameters and Method#inspect name a parameter the body assigns by
# the name the program wrote. Such a parameter is fed from a slot the
# compiler names (desugar_reassigned_block_params), and the slot's name was
# what printed, the slot's spelling, not `[[:req, :k]]`.
by_lambda = lambda { |k| k = k + 1; k }
by_proc = proc { |k, j| k = k.to_s; k + j.to_s }
mixed = lambda { |k, m = 2, *r, z| k = k + 1; z = z + m; k + z }
p by_lambda.parameters, by_proc.parameters, mixed.parameters
p by_lambda.parameters(lambda: false), by_proc.parameters(lambda: true)
p by_lambda.call(1), by_proc.call(1, 2), mixed.call(1, 2)

# under an outer local of the same name the parameter is renamed twice
k = 9
shadowing = lambda { |k| k = k * 2; k }
p shadowing.parameters, shadowing.call(2), k

# a name the program spells like a slot's stays its own
k__bpin = 5
beside = lambda { |k| k = k + 1; k + k__bpin }
own = lambda { |v__bpin| v__bpin }
p beside.call(1), beside.parameters, own.parameters

class Box
  define_method(:bump) { |k, j = 2| k = k + j; k }
end
p Box.new.method(:bump).inspect.sub(/ \S+:\d+>/, ">"), Box.new.bump(1)

# a long name is printed whole (129 bytes here; a fixed buffer cut it at 127)
long = lambda { |the_count_this_block_is_handed_under_a_name_longer_than_the_one_hundred_and_twenty_seven_bytes_a_fixed_buffer_once_cut_it_down_to| the_count_this_block_is_handed_under_a_name_longer_than_the_one_hundred_and_twenty_seven_bytes_a_fixed_buffer_once_cut_it_down_to = the_count_this_block_is_handed_under_a_name_longer_than_the_one_hundred_and_twenty_seven_bytes_a_fixed_buffer_once_cut_it_down_to + 1; the_count_this_block_is_handed_under_a_name_longer_than_the_one_hundred_and_twenty_seven_bytes_a_fixed_buffer_once_cut_it_down_to }
p long.call(1), long.parameters[0][1].to_s.size, long.parameters[0][1].to_s[-14, 14]
