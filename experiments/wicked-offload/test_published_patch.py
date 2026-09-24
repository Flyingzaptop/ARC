"""Prevent a local-only lifetime fix from disappearing from the delivered patch."""
from pathlib import Path
import unittest


class PublishedPatch(unittest.TestCase):
    def test_value_capture_and_post_join_oracle_are_published(self):
        patch=Path(__file__).with_name('wicked.patch').read_text()
        self.assertIn('+\t\twi::jobsystem::Dispatch(ctx, object_loop, groupSize, [&, replaced]',patch)
        self.assertNotIn('+\t\twi::jobsystem::Dispatch(ctx, object_loop, groupSize, [&]',patch)
        join=patch.index(' \twi::jobsystem::Wait(ctx);')
        oracle=patch.index('+    if(vis.flags & Visibility::ALLOW_OBJECTS) ARCControlledFinalList(',join)
        resize=patch.index(' \tvis.visibleObjects.resize((size_t)vis.object_counter.load());',oracle)
        self.assertLess(join,oracle)
        self.assertLess(oracle,resize)


if __name__=='__main__':unittest.main()
