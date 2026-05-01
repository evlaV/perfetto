import m from 'mithril';

import {DetailsShell} from '../../widgets/details_shell';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {SqlRef} from '../../widgets/sql_ref';
import {Tooltip} from '../../widgets/tooltip';
import {Checkbox} from '../../widgets/checkbox';
import {Button} from '../../widgets/button';

import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {QueryResult, STR} from '../../trace_processor/query_result';
import {Rect2D, Size2D, Point2D} from '../../base/geom';
import {download} from '../..//base/download_utils';

function extractNumbers(str: string): number[] {
  const matches = str.match(/\d+(\.\d+)?/g);
  return matches
    ? matches.map((v) => {
        return Math.floor(Number(v));
      })
    : [];
}

interface BinningProperties {
  sliceId: number;
  binSize: Size2D;
  renderedBinCount: number;
  unmergedBinCount: number;
  framebufferSize: Size2D;
  biggestMergedBins: number;
  biggestFDMScaling: Size2D;
  perViewViewport: boolean;
}

interface VisualizationRect {
  rect: Rect2D;
  fill: string /* Must be a hex encoded color. */;
  textColor: string;
  textCenter: string;
  textCenterSub: string;
  textTopLeft: string;
}

interface BinRect {
  rect: Rect2D;
  scale: Size2D;
}

interface BinLayoutGrid {
  grid: Rect2D;
  viewIdx: number;
  framebuffer: Rect2D;
  viewport: Rect2D;
  scissor: Rect2D;
  bins: Array<BinRect>;
}

interface BinLayoutInfoArgs {
  fbSize: Size2D;
  binSize: Size2D;
  binCoordinates: Array<Rect2D>;
  viewCount: number;
  perViewFDMScale: Array<Array<Size2D>>;
  perViewBinOffset: Array<Point2D>;
  viewports: Array<Rect2D>;
  scissors: Array<Rect2D>;
}

function fdmScaleIsNon0x0(scale: Size2D): boolean {
  if (scale.width == 0) return false;

  if (scale.height == 0) {
    throw new Error('Bin scale must be non-zero in both directions.');
  }

  return true;
}

class BinLayoutInfo {
  fbSize: Size2D;
  binSize: Size2D;
  binCoordinates: Array<Rect2D>;
  viewCount: number;
  perViewFDMScale: Array<Array<Size2D>>;
  perViewBinOffset: Array<Point2D>;
  viewports: Array<Rect2D>;
  scissors: Array<Rect2D>;

  constructor(args: BinLayoutInfoArgs) {
    this.fbSize = args.fbSize;
    this.binSize = args.binSize;
    this.binCoordinates = args.binCoordinates;
    this.viewCount = args.viewCount;
    this.perViewFDMScale = args.perViewFDMScale;
    this.perViewBinOffset = args.perViewBinOffset;
    this.viewports = args.viewports;
    this.scissors = args.scissors;
  }

  public static empty(): BinLayoutInfo {
    return new this({
      fbSize: {width: 0, height: 0},
      binSize: {width: 0, height: 0},
      binCoordinates: [],
      viewCount: 0,
      perViewFDMScale: [],
      perViewBinOffset: [],
      viewports: [],
      scissors: [],
    });
  }

  public static async fromSliceId(
    trace: Trace,
    sliceId: number,
  ): Promise<BinLayoutInfo> {
    const bin_layout_info_query = trace.engine.query(`
        SELECT key, string_value
        FROM args JOIN slice ON slice.arg_set_id=args.arg_set_id
        WHERE slice.id=${sliceId} AND
              (args.key IN ('width', 'height', 'binWidth', 'binHeight') OR
               args.key LIKE 'binCoordinates[_%]' OR
               args.key LIKE 'perViewFDMScale[_%][_%]' OR
               args.key LIKE 'perViewBinOffset[_%]' OR
               args.key LIKE 'viewports[_%]' OR
               args.key LIKE 'scissors[_%]')
      `);

    return bin_layout_info_query.then((r) =>
      BinLayoutInfo.query_table_to_json(r),
    );
  }

  private static query_table_to_json(query: QueryResult) {
    const it = query.iter({
      key: STR,
      string_value: STR,
    });

    let fbWidth = 0;
    let fbHeight = 0;
    let binWidth = 0;
    let binHeight = 0;
    const binCoordinates: Array<Rect2D> = [];
    const perViewFDMScale: Array<Array<Size2D>> = [];
    const perViewBinOffset: Array<Point2D> = [];
    const viewports: Array<Rect2D> = [];
    const scissors: Array<Rect2D> = [];

    for (; it.valid(); it.next()) {
      const literals = it.key.split('[');
      const arr_name = literals[0];

      let arr_idx: number | undefined = undefined;
      if (literals[1] !== undefined) {
        arr_idx = parseInt(literals[1].slice(0, -1));
      }

      let arr_idx_2: number | undefined = undefined;
      if (literals.length === 3) {
        arr_idx_2 = parseInt(literals[2].slice(0, -1));
      }

      switch (arr_name) {
        case 'width':
          fbWidth = +it.string_value;
          break;
        case 'height':
          fbHeight = +it.string_value;
          break;
        case 'binWidth':
          binWidth = +it.string_value;
          break;
        case 'binHeight':
          binHeight = +it.string_value;
          break;
        case 'binCoordinates': {
          const [x0, y0, x1, y1] = extractNumbers(it.string_value);
          binCoordinates[arr_idx!] = Rect2D.fromPoints(
            {x: x0, y: y0},
            {x: x1, y: y1},
          );
          break;
        }
        case 'perViewFDMScale': {
          if (arr_idx_2 === 0) {
            perViewFDMScale[arr_idx!] = [];
          }

          const [width, height] = extractNumbers(it.string_value);
          perViewFDMScale[arr_idx!][arr_idx_2!] = {width, height};
          break;
        }
        case 'perViewBinOffset':
          const [x, y] = extractNumbers(it.string_value);
          perViewBinOffset[arr_idx!] = {x, y};
          break;
        case 'viewports': {
          const [x0, y0, x1, y1] = extractNumbers(it.string_value);
          viewports[arr_idx!] = Rect2D.fromPoints(
            {x: x0, y: y0},
            {x: x1, y: y1},
          );
          break;
        }
        case 'scissors': {
          const [x0, y0, x1, y1] = extractNumbers(it.string_value);
          scissors[arr_idx!] = Rect2D.fromPoints(
            {x: x0, y: y0},
            {x: x1, y: y1},
          );
          break;
        }
      }
    }

    const viewCount = perViewFDMScale.length;
    if (viewCount < 1) throw new Error('There should be at least 1 view');

    /* Note: if fdm_offset isn't used we don't have and perViewBinOffset */
    if (perViewBinOffset.length > 0 && perViewBinOffset.length != viewCount) {
      throw new Error("perViewBinOffset doesn't have offsets for all views");
    }

    if (viewports.length != 1 && viewports.length != viewCount) {
      throw new Error(
        'There should be 1 viewport or a viewport for all views.',
      );
    }

    if (scissors.length != 1 && scissors.length != viewCount) {
      throw new Error(
        'There should be 1 scissors or a scissors for all views.',
      );
    }

    return new BinLayoutInfo({
      fbSize: {width: fbWidth, height: fbHeight} as Size2D,
      binSize: {width: binWidth, height: binHeight},
      binCoordinates,
      viewCount,
      perViewFDMScale,
      perViewBinOffset,
      viewports,
      scissors,
    });
  }

  private getBinnedArea(viewIdx: number): Size2D {
    let max_x = 0;
    let max_y = 0;

    for (let i = 0; i < this.binCoordinates.length; i++) {
      const coords = this.binCoordinates[i];
      const scale = this.perViewFDMScale[viewIdx][i];

      /* There can be bins with 0x0 scale within the binning grid itself too
       * such as for VR within the left and right eye.
       */
      if (!fdmScaleIsNon0x0(scale)) continue;

      if (coords.right > max_x) max_x = coords.right;

      if (coords.bottom > max_y) max_y = coords.bottom;
    }

    return {width: max_x, height: max_y};
  }

  public getBinningProperties(
    sliceId: number,
    viewIdx: number,
  ): BinningProperties {
    const unmergedBinGrid = size2DIntDiv(
      size2DAddScalar(this.getBinnedArea(viewIdx), 1),
      this.binSize,
    );
    const unmergedBinCount = size2DArea(unmergedBinGrid);

    const biggestMergedBins = this.binCoordinates
      .map((rect) =>
        size2DArea(size2DIntDiv(size2DAddScalar(rect, 1), this.binSize)),
      )
      .reduce((p, c) => Math.max(p, c));

    const biggestFDMScaling =
      this.perViewFDMScale[viewIdx].reduce(size2DGetBigger);

    const renderedBinCount =
      this.perViewFDMScale[viewIdx].filter(fdmScaleIsNon0x0).length;

    /* Note: with multiViewport we also have multiple viewports so we check that
     * we have multiple views too.
     */
    const perViewViewport = this.viewports.length > 1 && this.viewCount > 1;

    return {
      sliceId,
      binSize: this.binSize,
      renderedBinCount,
      unmergedBinCount,
      framebufferSize: this.fbSize,
      biggestMergedBins,
      biggestFDMScaling,
      perViewViewport,
    };
  }

  public getBinLayoutGrid(viewIdx: number): BinLayoutGrid {
    const gridArea = this.binCoordinates
      .map(({right, bottom}) => {
        return {width: right, height: bottom} as Size2D;
      })
      .reduce(size2DMax);

    const grid = Rect2D.fromPointAndSize({x: 0, y: 0, ...gridArea});
    const framebuffer = Rect2D.fromPointAndSize({x: 0, y: 0, ...this.fbSize});

    let bins: BinRect[] = [];
    for (let i = 0; i < this.binCoordinates.length; i++) {
      const rect = this.binCoordinates[i];
      const scale = this.perViewFDMScale[viewIdx][i];

      bins.push({rect, scale});
    }

    const binOffset = this.perViewBinOffset
      ? this.perViewBinOffset[viewIdx]
      : null;
    if (binOffset) {
      for (let col = 0; col < gridArea.width / this.binSize.width; col++) {
        bins.push({
          rect: Rect2D.fromPointAndSize({
            x: col * this.binSize.width,
            y: gridArea.height,
            ...this.binSize,
          }),
          scale: {width: 0, height: 0},
        });
      }

      for (let row = 0; row < gridArea.height / this.binSize.height; row++) {
        bins.push({
          rect: Rect2D.fromPointAndSize({
            x: gridArea.width,
            y: row * this.binSize.height,
            ...this.binSize,
          }),
          scale: {width: 0, height: 0},
        });
      }

      bins.push({
        rect: Rect2D.fromPointAndSize({
          x: gridArea.width,
          y: gridArea.height,
          ...this.binSize,
        }),
        scale: {width: 0, height: 0},
      });

      bins = bins.map(({rect, scale}) => {
        rect = rect.translate({x: -binOffset.x, y: -binOffset.y});
        return {rect, scale};
      });
    }

    /* Accounting for VK_QCOM_multiview_per_view_viewports. */
    const vp_sc_idx = this.viewports.length == 1 ? 0 : viewIdx;

    const viewport = this.viewports[vp_sc_idx];
    const scissor = this.scissors[vp_sc_idx];

    return {grid, viewIdx, framebuffer, viewport, scissor, bins};
  }
}

function size2DAddScalar(a: Size2D, b: number): Size2D {
  return {width: a.width + b, height: a.height + b};
}

function size2DIntDiv(a: Size2D, divisor: Size2D): Size2D {
  return {
    width: Math.floor(a.width / divisor.width),
    height: Math.floor(a.height / divisor.height),
  };
}

function size2DArea(a: Size2D): number {
  return a.width * a.height;
}

function size2DToText(size: Size2D) {
  return `(${size.width}, ${size.height})`;
}

function size2DGetBigger(a: Size2D, b: Size2D): Size2D {
  return size2DArea(a) > size2DArea(b) ? a : b;
}

function size2DMax(a: Size2D, b: Size2D): Size2D {
  return {
    width: Math.max(a.width, b.width),
    height: Math.max(a.height, b.height),
  };
}

function rect2DRelativeCenter(rect: Rect2D): Point2D {
  return {x: rect.width / 2, y: rect.height / 2};
}

class BinningPropertiesPanel implements m.ClassComponent<BinningProperties> {
  view({attrs}: m.Vnode<BinningProperties>) {
    const binReduction =
      (attrs.unmergedBinCount - attrs.renderedBinCount) /
      attrs.unmergedBinCount;

    interface Property {
      title: string;
      description?: string;
      value: m.Child;
    }

    const properties: Array<Property> = [
      {
        title: 'SQL ID',
        value: m(SqlRef, {table: 'slice', id: attrs.sliceId}),
      },
      {
        title: 'VK_QCOM_multiview_per_view_viewports',
        description: 'If per view viewports and scissors are being used or not',
        value: m(Checkbox, {
          checked: attrs.perViewViewport,
          style: {pointerEvents: 'none'},
        }),
      },
      {
        title: 'Bin size',
        description: 'Size of an un-merged bin (W, H)',
        value: size2DToText(attrs.binSize),
      },
      {
        title: 'Bin count (merged)',
        description: 'Count of rendered bins, accounting for merged bins.',
        value: attrs.renderedBinCount,
      },
      {
        title: 'Bin count (unmerged)',
        description: 'Count of rendered bins if no bins were merged. ',
        value: `${attrs.unmergedBinCount}`,
      },
      {
        title: 'Bin reduction',
        description:
          'Percentage reduction in rendered bin count due to bin merging',
        value: `${(binReduction * 100).toFixed(2)} %`,
      },
      {
        title: 'Biggest merged bins',
        description: 'Count of bins merged to form the biggest merged bin',
        value: `${attrs.biggestMergedBins}`,
      },
      {
        title: 'Biggest scaling',
        description: 'Biggest scale applied to a bin',
        value: `${attrs.biggestFDMScaling.width} x ${attrs.biggestFDMScaling.height}`,
      },
      {
        title: 'Framebuffer size',
        description: 'Size of the framebuffer (W, H)',
        value: size2DToText(attrs.framebufferSize),
      },
    ];

    return m(
      Tree,
      {className: 'pf-view-prop-list'},
      properties.map((p) => {
        if (!p.description) return m(TreeNode, {left: p.title, right: p.value});

        return m(TreeNode, {
          left: m(
            Tooltip,
            {
              showArrow: false,
              offset: 10,
              trigger: m('span', p.title),
            },
            p.description,
          ),
          right: p.value,
        });
      }),
    );
  }
}

class BinningVisualization implements m.ClassComponent<BinLayoutGrid> {
  private static readonly TEXT_CONTRAST = 0.3;
  /* How much to darken parts outside the framebuffer. */
  private static readonly FB_OUTSIDE_DARKEN = 0.5;
  /* How much to darken parts outside the scissor but within the framebuffer. */
  private static readonly SC_OUTSIDE_DARKEN = 0.3;

  public static getSVGSelector(viewIdx: number): string {
    return `pf-svg-${viewIdx}`;
  }

  private static getTextColor(bgColor: string): string {
    bgColor = bgColor.substring(1); /* Remove "#" */

    const color = parseInt(bgColor, 16);
    const R = ((color >> 16) & 255) / 255;
    const G = ((color >> 8) & 255) / 255;
    const B = (color & 255) / 255;

    /* See https://stackoverflow.com/a/596243, although it might not be the
     * correct answer, it's good enough for this.
     */
    const luminance = 0.299 * R + 0.587 * G + 0.114 * B;

    return luminance < BinningVisualization.TEXT_CONTRAST ? '#FFF' : '#000';
  }

  private static binRectToVisualizationRect(
    binRect: BinRect,
  ): VisualizationRect {
    let fill = '';

    if (binRect.scale.width === 0) {
      fill = '#FFFFFFFF';
    } else {
      const color = [1 / binRect.scale.width, 1 / binRect.scale.height, 0, 1];

      fill =
        '#' +
        color
          .map((comp) =>
            Math.floor(255 * comp)
              .toString(16)
              .padStart(2, '0'),
          )
          .join('');
    }

    const textColor = BinningVisualization.getTextColor(fill);

    const rect = binRect.rect;

    return {
      rect,
      fill,
      textColor,
      textCenter: `Scale ${binRect.scale.width} x ${binRect.scale.height}`,
      textCenterSub: `Bin size: (${rect.width + 1}, ${rect.height + 1})`,
      textTopLeft: `(${rect.x}, ${rect.y})`,
    };
  }

  view({attrs}: m.Vnode<BinLayoutGrid>) {
    /* Controls how much the labels on the edge of a bin should be pulled in
     * towards the center.
     */
    const edgeLabelYPadding = '0.7em';

    return m(
      `svg#${BinningVisualization.getSVGSelector(attrs.viewIdx)}`,
      {
        style: {
          position: 'unset',
          padding: '5px',
          backgroundColor: '#000',
          pointerEvents: 'auto' /* Needed to show tooltips. */,
          overflow: 'hidden' /* Clips bins outside the SVG with FDM offset. */,
        },
        /* See: https://css-tricks.com/scale-svg/ */
        viewBox: `0 0 ${attrs.grid.width} ${attrs.grid.height}`,
        preserveAspectRatio: 'xMidYMid meet',
        xmlns: 'http://www.w3.org/2000/svg',
      },

      /* Bins */
      attrs.bins
        .map(BinningVisualization.binRectToVisualizationRect)
        .map((v_rect) => {
          const textColor = v_rect.textColor
            ? v_rect.textColor
            : BinningVisualization.getTextColor(v_rect.fill);

          return m(
            'g',
            {transform: `translate(${v_rect.rect.x},${v_rect.rect.y})`},
            [
              /* Tooltip */
              v_rect.textCenter &&
                m(
                  'title',
                  [
                    v_rect.textTopLeft,
                    v_rect.textCenter,
                    v_rect.textCenterSub,
                  ].join('\n'),
                ),

              /* Rectangle */
              m('rect', {
                width: v_rect.rect.width,
                height: v_rect.rect.height,
                fill: v_rect.fill,
                stroke: '#000',
              }),

              /* Centered text */
              v_rect.textCenter &&
                m(
                  'text.label',
                  {
                    ...rect2DRelativeCenter(v_rect.rect),
                    'dominant-baseline': 'middle',
                    'text-anchor': 'middle',
                    'font-size': '1.5em',
                    'fill': textColor,
                  },
                  v_rect.textCenter,
                ),
              /* Centered sub-text */
              v_rect.textCenterSub &&
                m(
                  'text.label',
                  {
                    ...rect2DRelativeCenter(v_rect.rect),
                    'dy': '1.5em',
                    'dominant-baseline': 'middle',
                    'text-anchor': 'middle',
                    'fill': textColor,
                  },
                  v_rect.textCenterSub,
                ),

              /* Top‑left text */
              v_rect.textTopLeft &&
                m(
                  'text.label',
                  {
                    'x': '0.5em',
                    'y': edgeLabelYPadding,
                    'dominant-baseline': 'hanging',
                    'fill': textColor,
                  },
                  v_rect.textTopLeft,
                ),
            ],
          );
        }),

      /* Viewport */
      m(
        'g',
        {transform: `translate(${attrs.viewport.x},${attrs.viewport.y})`},
        m('rect', {
          width: attrs.viewport.width,
          height: attrs.viewport.height,
          fill: 'none',
          stroke: '#F00A',
        }),
        m(
          'text.label',
          {
            'x': '-0.5em',
            'y': edgeLabelYPadding,
            'dx': attrs.viewport.width,
            'dominant-baseline': 'hanging',
            'text-anchor': 'end',
            'fill': '#F00',
            'style': {fontSize: '2em'},
          },
          'Viewport',
        ),
      ),

      /* Scissor */
      m(
        'g',
        {
          'pointer-events': 'none' /* Needed for the bin tooltips to show. */,
        },
        m(
          `mask#pf-sc-mask-${attrs.viewIdx}`,
          m('rect', {width: '100%', height: '100%', fill: 'white'}),
          m('rect', {
            x: attrs.scissor.x,
            y: attrs.scissor.y,
            width: attrs.scissor.width,
            height: attrs.scissor.height,
            fill: 'black',
            strokeWidth: 0,
          }),
        ),
        m('rect', {
          x: attrs.framebuffer.x,
          y: attrs.framebuffer.y,
          width: attrs.framebuffer.width,
          height: attrs.framebuffer.height,
          fill: `rgba(0, 0, 0, ${BinningVisualization.SC_OUTSIDE_DARKEN})`,
          mask: `url(#pf-sc-mask-${attrs.viewIdx})`,
          strokeWidth: 0,
        }),
        m('rect', {
          x: attrs.scissor.x,
          y: attrs.scissor.y,
          width: attrs.scissor.width,
          height: attrs.scissor.height,
          fill: 'none',
          stroke: '#00FA',
        }),
        m(
          'text.label',
          {
            'x': '-0.5em',
            'y': '-' + edgeLabelYPadding,
            'dx': attrs.scissor.x + attrs.scissor.width,
            'dy': attrs.scissor.y + attrs.scissor.height,
            'text-anchor': 'end',
            'fill': '#00F',
            'style': {fontSize: '2em'},
          },
          'Scissor',
        ),
      ),

      /* Framebuffer */
      m(
        'g',
        {
          'pointer-events': 'none' /* Needed for the bin tooltips to show. */,
        },
        m(
          `mask#pf-fb-mask-${attrs.viewIdx}`,
          m('rect', {width: '100%', height: '100%', fill: 'white'}),
          m('rect', {
            x: attrs.framebuffer.x,
            y: attrs.framebuffer.y,
            width: attrs.framebuffer.width,
            height: attrs.framebuffer.height,
            fill: 'black',
            strokeWidth: 0,
          }),
        ),
        m('rect', {
          width: attrs.grid.width,
          height: attrs.grid.height,
          fill: `rgba(0, 0, 0, ${BinningVisualization.FB_OUTSIDE_DARKEN})`,
          mask: `url(#pf-fb-mask-${attrs.viewIdx})`,
          strokeWidth: 0,
        }),
      ),
      m(
        'g',
        {
          transform: `translate(${attrs.framebuffer.width},${attrs.framebuffer.height})`,
        },
        m('line', {x1: '0', y1: '-10', x2: '0', y2: '10', stroke: '#0F0'}),
        m('line', {x1: '-10', y1: '0', x2: '10', y2: '0', stroke: '#0F0'}),
        m(
          'text.label',
          {
            'dx': '0.5em',
            'dy': '0.5em',
            'dominant-baseline': 'hanging',
            'fill': '#0F0',
            'style': {fontSize: '2em'},
          },
          'FB',
        ),
      ),
    );
  }
}

export class BinLayoutSliceDetailsPanel implements TrackEventDetailsPanel {
  private slice_id: number = 0;

  constructor(
    private readonly trace: Trace,
    private layout_info: BinLayoutInfo | null = null,
  ) {}

  async load(selection: TrackEventSelection): Promise<void> {
    if (selection.kind != 'track_event') return;

    this.slice_id = selection.eventId;

    return BinLayoutInfo.fromSliceId(this.trace, this.slice_id).then(
      (layout) => {
        this.layout_info = layout;
        return;
      },
    );
  }

  render() {
    if (this.layout_info === null) {
      return m(DetailsShell, {title: 'Bin Layout'}, 'Loading...');
    }

    const info = this.layout_info;
    const views = info.viewCount > 1 ? [0, 1] : [0];

    const gridTemplateColumns = '1fr '.repeat(views.length) + 'auto';

    return m(
      DetailsShell,
      {
        title: 'Bin Layout',
        buttons: views.map((viewIdx) => {
          const svgId = BinningVisualization.getSVGSelector(viewIdx);
          const fileName = `pf-bin-layout-${viewIdx}.svg`;

          return m(Button, {
            label: `View ${viewIdx}`,
            icon: 'download',
            onclick: () => {
              const svgElement = document.getElementById(svgId);
              if (!svgElement) return;

              const content = svgElement.outerHTML;

              download({
                content,
                mimeType: 'image/svg+xml',
                fileName,
              });
            },
          });
        }),
      },
      m(
        'div.pf-grid-layout',
        {style: {gridTemplateColumns}},
        views.map((viewIdx) => {
          return m(
            Section,
            {
              title: `View ${viewIdx}`,
            },
            m(BinningVisualization, info.getBinLayoutGrid(viewIdx)),
          );
        }),
        m(
          'div.pf-view-prop-panels',
          views.map((viewIdx) => {
            return m(
              Section,
              {title: `Properties View ${viewIdx}`},
              m(
                BinningPropertiesPanel,
                info.getBinningProperties(this.slice_id, viewIdx),
              ),
            );
          }),
        ),
      ),
    );
  }
}
