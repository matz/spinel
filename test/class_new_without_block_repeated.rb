# A class factory calling Class.new(parent) without a block makes a
# different class at each call.
class NoBlockRepeatedBase; end
def no_block_factory = Class.new(NoBlockRepeatedBase)
p no_block_factory == no_block_factory
