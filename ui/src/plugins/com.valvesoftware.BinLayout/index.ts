import type {Trace} from '../../public/trace';
import type {App} from '../../public/app';
import type {PerfettoPlugin} from '../../public/plugin';
import {SliceTrack} from '../../components/tracks/slice_track';
import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {SourceDataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';
import {BinLayoutSliceDetailsPanel} from './bin_layout_details_panel';
import type {RouteArgs} from '../../public/route_schema';

import {LONG, NUM, STR} from '../../trace_processor/query_result';

export default class BinLayout implements PerfettoPlugin {
  static readonly id = 'com.valvesoftware.BinLayout';
  static readonly description = 'Visualize GPU binning layout';

  static onActivate(_app: App, args: RouteArgs): void {
    console.log('SkeletonPlugin::onActivate()');
    console.log(`Route args for ${BinLayout.id}`, args);
  }

  async onTraceLoad(
    trace: Trace,
    _args: {[key: string]: unknown},
  ): Promise<void> {
    const tableCreate = createPerfettoTable({
      engine: trace.engine,
      name: 'bin_layout_tiled_render_arg_sets',
      as: `
        SELECT arg_set_id FROM args
        WHERE key='tiledRender' AND string_value='true'
      `,
    });

    const versionQuery = trace.engine.query(`
      SELECT EXISTS (
        SELECT 1 FROM args WHERE key='tiledRender' AND string_value='true'
      ) AS hasBinLayout
    `);
    const row = (await versionQuery).firstRow({hasBinLayout: NUM});
    const hasBinLayout = Boolean(row.hasBinLayout);

    if (!hasBinLayout) return;

    await tableCreate;

    const uri = `com.valvesoftware.BinLayout#Track`;
    trace.tracks.registerTrack({
      uri,
      renderer: SliceTrack.create({
        trace: trace,
        uri,
        dataset: new SourceDataset({
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG,
            name: STR,
            depth: NUM,
          },
          select: {
            id: 'id',
            ts: 'ts',
            dur: 'dur',
            name: 'name',
            depth: 'depth',
          },
          src: 'slice',
          joins: {
            bin_layout_tiled_render_arg_sets: {
              from: 'bin_layout_tiled_render_arg_sets USING (arg_set_id)',
            },
          },
        }),
        detailsPanel: () => new BinLayoutSliceDetailsPanel(trace),
      }),
    });
    const track = new TrackNode({
      uri,
      name: `GPU Bin-layout`,
    });
    trace.defaultWorkspace.addChildInOrder(track);
  }
}
