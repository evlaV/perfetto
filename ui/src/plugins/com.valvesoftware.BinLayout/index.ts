import {Trace} from '../../public/trace';
import {App} from '../../public/app';
import {PerfettoPlugin} from '../../public/plugin';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';
import {BinLayoutSliceDetailsPanel} from './bin_layout_details_panel'
import {RouteArgs} from '../../public/route_schema';


import {
  LONG,
  NUM,
  STR,
} from '../../trace_processor/query_result';



export default class BinLayout implements PerfettoPlugin {
  static readonly id = 'com.valvesoftware.BinLayout';
  static readonly description = 'Visualize GPU binning layout';

  static onActivate(_app: App, args: RouteArgs): void {
    console.log('SkeletonPlugin::onActivate()');
    console.log(`Route args for ${BinLayout.id}`, args)
  }

  async onTraceLoad(trace: Trace, _args: {[key: string]: unknown}): Promise<void> {
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
          src: `
            SELECT slice.id, ts, dur, name, depth FROM slice
            JOIN args ON args.arg_set_id=slice.arg_set_id
            WHERE key='tiledRender' AND string_value='true'
          `
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
