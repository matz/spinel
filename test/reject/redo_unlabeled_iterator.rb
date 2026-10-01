# `redo` re-runs a block's body without binding its parameters again. A
# stage of a lazy pipeline is walked by an emitter that places no label for
# it, and the redo compiled to a `continue`, which left the block as `next`
# does: CRuby answers [18, 4], Spinel answered [4]. It is refused at this
# line instead (test/redo_block_keeps_writes.rb,
# test/builtin_iter_step_frame.rb and test/builtin_iter_step_frame_more.rb
# have the iterators that run it).
done = false
p([1, 2].lazy.map { |x| unless done; done = true; x = 9; redo; end; x * 2 }.to_a)
