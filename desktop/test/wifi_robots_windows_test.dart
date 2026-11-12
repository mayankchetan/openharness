// Two things about the Devices tab's robots on Windows, both without a real network or CLI:
//  - the words and the button: Windows Firewall, not a Mac's Local Network switch;
//  - the polling: a hidden app (minimised, occluded) asks nothing, and one read at once when it is shown again.
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:harness/auth/auth_session.dart';
import 'package:harness/autonomous_device/autonomous_device_cli.dart';
import 'package:harness/core/config.dart';
import 'package:harness/devices/devices_controller.dart';
import 'package:harness/devices/devices_screen.dart';
import 'package:harness/devices/wifi_robots.dart';
import 'package:harness/state/app_state.dart';
import 'package:harness/state/dial_status.dart';

import 'wifi_robots_test.dart' show FakeAutonomousDeviceCli;

class _DevicesApp extends AppNotifier {
  _DevicesApp()
    : super(
        config: AppConfig.dev,
        authSession: AuthSession(),
        configStore: null,
      );
  int reads = 0;
  @override
  Future<void> refreshDevices() async => reads++;
}

/// Well before any interval: what "at once" means here.
Future<void> _soon(WidgetTester tester) async {
  for (var i = 0; i < 5; i++) {
    await tester.pump(const Duration(milliseconds: 20));
  }
}

void _lifecycle(WidgetTester tester, AppLifecycleState state) =>
    tester.binding.handleAppLifecycleStateChanged(state);

/// Shown, hidden for three intervals, shown again.
Future<void> _expectQuietWhileHidden(
  WidgetTester tester, {
  required Duration every,
  required int Function() reads,
}) async {
  addTearDown(() => _lifecycle(tester, AppLifecycleState.resumed));
  var before = reads();
  await tester.pump(every);
  await _soon(tester);
  expect(reads(), before + 1, reason: 'reads on its interval while shown');

  // Unfocused, still in sight: it keeps reading.
  _lifecycle(tester, AppLifecycleState.inactive);
  await tester.pump();
  before = reads();
  await tester.pump(every);
  await _soon(tester);
  expect(reads(), before + 1, reason: 'an unfocused window still reads');

  _lifecycle(tester, AppLifecycleState.hidden);
  await tester.pump();
  before = reads();
  await tester.pump(every * 3);
  await _soon(tester);
  expect(reads(), before, reason: 'no request while the app is hidden');

  _lifecycle(tester, AppLifecycleState.inactive);
  _lifecycle(tester, AppLifecycleState.resumed);
  await _soon(tester);
  expect(reads(), before + 1, reason: 'one read at once when it is shown');
  await tester.pump(every);
  await _soon(tester);
  expect(reads(), before + 2, reason: 'then on its interval again');
}

Widget _host(Widget child) => MaterialApp(home: Scaffold(body: child));

/// A button that opens the Wi-Fi code dialog over [robots].
Widget _opener(WifiRobotsController robots, {void Function()? openSettings}) =>
    MaterialApp(
      home: Scaffold(
        body: Builder(
          builder: (context) => TextButton(
            key: const Key('open-wifi'),
            onPressed: () => showWifiPairDialog(
              context,
              robots,
              openLocalNetworkSettings: openSettings,
            ),
            child: const Text('Wi-Fi code'),
          ),
        ),
      ),
    );

Future<void> _openDialog(
  WidgetTester tester,
  FakeAutonomousDeviceCli cli, {
  void Function()? openSettings,
}) async {
  final robots = WifiRobotsController(cli: cli)..start();
  addTearDown(robots.dispose);
  await tester.pumpWidget(_opener(robots, openSettings: openSettings));
  await tester.pumpAndSettle();
  await tester.tap(find.byKey(const Key('open-wifi')));
  await tester.pumpAndSettle();
}

void main() {
  group('on Windows', () {
    testWidgets(
      'a blocked network names Windows Firewall and offers the firewall settings',
      (tester) async {
        var opened = 0;
        final cli = FakeAutonomousDeviceCli()..networkBlocked = true;
        await _openDialog(tester, cli, openSettings: () => opened++);
        expect(
          find.byKey(const Key('autonomous-device-network-blocked')),
          findsOneWidget,
        );
        expect(
          find.textContaining('Windows Firewall is blocking Harness'),
          findsOneWidget,
        );
        expect(find.textContaining('macOS'), findsNothing);
        expect(find.text('Open Settings'), findsNothing);
        await tester.tap(find.text('Open firewall settings'));
        expect(opened, 1);
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );

    testWidgets(
      'an empty list says the firewall may be the reason',
      (tester) async {
        await _openDialog(tester, FakeAutonomousDeviceCli()..discovered = []);
        expect(
          find.textContaining('Windows Firewall may be blocking Harness'),
          findsOneWidget,
        );
      },
      variant: TargetPlatformVariant.only(TargetPlatform.windows),
    );

    test('the CLI refusal reads in Windows words', () {
      debugDefaultTargetPlatformOverride = TargetPlatform.windows;
      addTearDown(() => debugDefaultTargetPlatformOverride = null);
      const refusal = AutonomousDeviceCliException(
        'LOCAL_NETWORK_BLOCKED',
        'refused',
      );
      expect(refusal.userMessage, contains('Windows Firewall'));
      expect(refusal.userMessage, isNot(contains('System Settings')));
    });
  });

  testWidgets('off Windows the words stay the Mac ones', (tester) async {
    await _openDialog(tester, FakeAutonomousDeviceCli()..discovered = []);
    expect(find.textContaining('Windows Firewall'), findsNothing);
    expect(find.textContaining('No Autonomous robots found.'), findsOneWidget);
  });

  testWidgets(
    'devices tab: hidden app, no request for 3 intervals; shown again, one read at once',
    (tester) async {
      final app = _DevicesApp();
      addTearDown(app.dispose);
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1280, 900);
      addTearDown(tester.view.reset);
      await tester.pumpWidget(
        _host(
          DevicesTab(notifier: app, refreshEvery: const Duration(seconds: 10)),
        ),
      );
      await _soon(tester);
      expect(app.reads, 1);
      await _expectQuietWhileHidden(
        tester,
        every: const Duration(seconds: 10),
        reads: () => app.reads,
      );
      await tester.pumpWidget(const SizedBox());
    },
  );

  testWidgets(
    'wifi robots: hidden app, no request for 3 intervals; shown again, one read at once',
    (tester) async {
      final cli = FakeAutonomousDeviceCli();
      final wifi = WifiRobotsController(
        cli: cli,
        refreshEvery: const Duration(seconds: 60),
      );
      final dial = DialState();
      addTearDown(dial.dispose);
      final controller = DevicesController(
        dial: dial,
        accountId: 'a',
        sendSettings: (_, _) async => true,
      );
      addTearDown(controller.dispose);
      tester.view.devicePixelRatio = 1;
      tester.view.physicalSize = const Size(1280, 900);
      addTearDown(tester.view.reset);
      await tester.pumpWidget(
        _host(DevicesScreen(controller: controller, wifi: wifi)),
      );
      await _soon(tester);
      expect(cli.statusCalls, 1);
      await _expectQuietWhileHidden(
        tester,
        every: const Duration(seconds: 60),
        reads: () => cli.statusCalls,
      );
      await tester.pumpWidget(const SizedBox());
      wifi.dispose();
    },
  );
}
