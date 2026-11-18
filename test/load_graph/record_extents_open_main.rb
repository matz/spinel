# A closure's extent comes from the calls that invoke it only while the
# program reaches it by the reads its syntax shows. A send of const_get,
# under any send name or a computed method name, reaches a constant by a
# name it computes, so every lambda a constant holds starts where its
# write runs and runs any number of times; a local read through binding
# opens the local the same way.
NEVER = -> { p :never }
LATER = -> { p :later }
Object.send(:const_get, :LATER).call
Object.__send__(:const_get, "LATER").()
n = :const_get
Object.public_send(n, :LATER).call
l = -> { p :local }
binding.local_variable_get(:l).call
