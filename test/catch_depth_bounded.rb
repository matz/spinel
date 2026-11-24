# The same bound on the catch stack, reached the same dynamic way: each level
# holds its catch open across the recursive call, so a recursion with no end
# pushes one entry per level until the array is full. SystemStackError, as
# for the exception frames (test/exc_frame_depth_bounded.rb); unrescued here,
# so CRuby's words on stderr and exit 1, which is what CRuby does when its own
# stack runs out. Silent on stdout either way.
def deep_catch(n)
  catch(:t) { deep_catch(n + 1) + 1 }
end
deep_catch(0)
puts "unreachable"
