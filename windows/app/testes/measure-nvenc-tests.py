"""Checks for the measurement's arithmetic and invalid-data rejection."""
import csv
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('measure', Path(__file__).with_name('measure-nvenc.py'))
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)


class MeasurementChecks(unittest.TestCase):
    def test_known_frame_series(self):
        # One second, 60 frames, 8 Mbit/s exactly, with an IDR and low-quality recovery.
        measure.RESULTS.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=measure.RESULTS) as temp:
            prefix = Path(temp) / 'fixture'
            with prefix.with_suffix('.csv').open('w', newline='') as out:
                writer = csv.writer(out)
                writer.writerow(['frame','psnr_y','encode_ms','bytes','key','brightness','motion'])
                for i in range(60):
                    writer.writerow([i, 20 if i==0 else 30 if i<=5 else 40,
                                     20 if i==59 else 2, 1000000/60, int(i==0), 50, 5])
            prefix.with_suffix('.meta').write_text('frames=60\nbitrate=8000000\n')
            r = measure.load(prefix)
            self.assertAlmostEqual(r['mbps'], 8)
            self.assertAlmostEqual(r['burst_ratio'], 1)
            self.assertAlmostEqual(r['ratio'], 1)
            self.assertEqual(r['idr'], 20)
            self.assertEqual(r['post5'], 30)
            self.assertEqual(r['p5'], 30)
            self.assertEqual(r['ms95'], 2)
            self.assertEqual(r['msmax'], 20)
            self.assertEqual(r['slow'], 1)
            self.assertAlmostEqual(r['psnr'], (20+5*30+54*40)/60)
            # A pooled percentile must come from all frames, not the mean of
            # the percentiles (30 here, versus an incorrect 31).
            with prefix.with_suffix('.csv').open() as source:
                data = list(csv.DictReader(source))
            runs = []
            for repeat in (1,2):
                target = Path(temp) / f'fixture-8-hoje-r{repeat}'
                with target.with_suffix('.csv').open('w', newline='') as out:
                    writer = csv.DictWriter(out, fieldnames=data[0].keys())
                    writer.writeheader()
                    for row in data:
                        record = dict(row)
                        record['psnr_y'] = float(row['psnr_y']) + (repeat-1)*2
                        if repeat == 2 and row['frame'] == '59':
                            record['encode_ms'] = 30
                        writer.writerow(record)
                target.with_suffix('.meta').write_text('frames=60\nbitrate=8000000\n')
                runs.append(measure.load(target))
            with patch.object(measure, 'RESULTS', Path(temp)):
                combined = measure.pooled('fixture',8,'hoje',runs)
            self.assertEqual(combined['frames'],120)
            self.assertEqual(combined['p5'],30)
            self.assertAlmostEqual(combined['psnr'],r['psnr']+1)
            self.assertEqual(combined['slow'],2)
            self.assertEqual(combined['msmax'],30)
            self.assertAlmostEqual(combined['mbps'],8)
            # A missing frame must not silently improve quality or bitrate.
            text = prefix.with_suffix('.csv').read_text()
            prefix.with_suffix('.csv').write_text(text.replace('10,40,', '11,40,'))
            with self.assertRaises(AssertionError):
                measure.load(prefix)

    def test_percentile_tail(self):
        self.assertEqual(measure.quantile(list(range(1,101)), .05), 5)
        self.assertEqual(measure.quantile(list(range(1,101)), .95), 95)


if __name__ == '__main__':
    unittest.main()

