# A program whose comment and String spell DATA but whose code reads no
# DATA keeps its data section out of the program
puts "DATA"
puts "after"
__END__
not read
