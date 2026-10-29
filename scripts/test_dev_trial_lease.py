import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import MagicMock, patch

import dev_trial_lease as trial


class TrialCLI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.config = self.root/'config.json'
        self.config.write_text(json.dumps({'host': '192.168.1.100', 'deviceID': '1234ABCD'}))
        self.image = self.root/'production.bin'
        data = b'\xe9'+b'\0'*100+b'PocketDaily-ESP32-0.1.0\0'+trial.CAPABILITY
        self.image.write_bytes(data.ljust(65536, b'\0'))
        self.directory = self.root/'plan'
        self.plan_args = argparse.Namespace(config=self.config, firmware=self.image, model='X3',
            expected_version='0.1.0-dev-main-w12345678', seconds=30, output=self.directory)
        trial.plan(self.plan_args)
        self.path = self.directory/'plan.private.json'
        self.p = json.loads(self.path.read_text())
        self.client = MagicMock()
        self.status = {'version': self.p['expectedVersion'], 'device': 'X3', 'mode': 'STA',
                       'deviceID': self.p['deviceID'], 'uptime': 40}

    def args(self, command, **kwargs):
        values = dict(command=command, plan=self.path, output=self.root/command, execute=True,
                      mode='sync', return_image=None, return_backup=None, backup_receipt=None, transfer=False)
        values.update(kwargs)
        return argparse.Namespace(**values)

    def reader_plan(self):
        self.plan_args.output = self.root/'reader-plan'
        self.plan_args.reader_epub = self.root/'fixture.epub'
        self.plan_args.reader_epub.write_bytes(b'PK-reader-fixture-bytes')
        self.image.write_bytes(self.image.read_bytes()+b'POCKET_TRIAL_READER_V1')
        trial.plan(self.plan_args)
        path = self.plan_args.output/'plan.private.json'
        return path, trial.checked(path)[0]

    def test_reader_fixture_is_frozen_and_hash_bound(self):
        path, p = self.reader_plan()
        self.assertEqual(p['fixture'], '/Pocket trial '+str(p['run'])+'.epub')
        self.assertEqual(p['readerPages'], 20)
        (path.parent/'reader.epub').write_bytes(b'PK-other')
        with self.assertRaises(ValueError): trial.checked(path)

    def test_reader_plan_requires_production_reader_capability(self):
        self.plan_args.output = self.root/'reader-without-cap'
        self.plan_args.reader_epub = self.root/'fixture.epub'
        self.plan_args.reader_epub.write_bytes(b'PK-reader')
        with self.assertRaises(ValueError): trial.plan(self.plan_args)
        self.assertFalse(self.plan_args.output.exists())

    def test_reader_terminal_requires_every_step_and_actual_timer_wake(self):
        path, p = self.reader_plan()
        good = {'present':True, 'run':p['run'], 'phase':4, 'failure':0,
                'trialSHA256':p['imageSHA256'], 'resetReason':8, 'wakeCause':4,
                'reader':{'schema':1,'run':p['run'],'stage':7,'requested':20,'rendered':20,
                          'reopened':1,'missingObserved':1,'recovered':1,'memorySamples':20,'renderMemorySamples':20,'minFreeHeap':60000,'minLargestBlock':40000}}
        for n, key in enumerate(('reopened','missingObserved','recovered','rendered','stage')):
            record = {**good, 'reader':{**good['reader'],key:0}}
            self.client.get_json.return_value = record
            out = self.root/('reader-failed-'+str(n)); out.mkdir()
            with patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError):
                trial.observe(self.client,p,out,False)
        self.client.get_json.return_value = good
        out = self.root/'reader-pass'; out.mkdir()
        with patch.object(trial,'status',return_value=self.status): trial.observe(self.client,p,out,False)
        self.assertTrue(json.loads((out/'receipt.json').read_text())['completed'])
        for n, overrides in enumerate(({'resetReason':3},{'wakeCause':7})):
            self.client.get_json.return_value = {**good,**overrides}
            out = self.root/('wrong-wake-'+str(n)); out.mkdir()
            with patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError):
                trial.observe(self.client,p,out,False)

    def test_auto_wake_requires_capability_budget_and_each_evidence_field(self):
        self.plan_args.reader_auto_wake = True
        self.plan_args.seconds = 360
        self.image.write_bytes(self.image.read_bytes()+b'POCKET_TRIAL_AUTOWAKE_V1')
        path, p = self.reader_plan()
        self.assertTrue(p['readerAutoWake'])
        wake = dict(schema=1, stage=6, timeoutMs=60000, idleMs=60011,
                    resetReason=8, wakeCause=4, positionMatched=1, pageAdvanced=1)
        good = dict(present=True, run=p['run'], phase=4, failure=0,
                    trialSHA256=p['imageSHA256'], resetReason=3, wakeCause=0,
                    reader=dict(schema=1,run=p['run'],stage=7,requested=20,rendered=20,
                                reopened=1,missingObserved=1,recovered=1,memorySamples=20,renderMemorySamples=20,minFreeHeap=60000,minLargestBlock=40000,autoWake=wake))
        for n, (key, wrong) in enumerate(dict(schema=0,stage=3,timeoutMs=1,idleMs=59999,
                                             resetReason=3,wakeCause=8,positionMatched=0,pageAdvanced=0).items()):
            bad = {**good, 'reader':{**good['reader'], 'autoWake':{**wake,key:wrong}}}
            self.client.get_json.return_value = bad
            out = self.root/('bad-auto-'+str(n)); out.mkdir()
            with patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError):
                trial.observe(self.client,p,out,False)
        self.client.get_json.return_value = good
        out = self.root/'auto-pass'; out.mkdir()
        with patch.object(trial,'status',return_value=self.status): trial.observe(self.client,p,out,False)
        self.assertTrue(json.loads((out/'receipt.json').read_text())['completed'])

    def test_file_transfer_plan_is_bounded_and_fixture_source_bound(self):
        self.plan_args.output = self.root/'ft-plan'
        self.plan_args.surface = 'file-transfer'
        self.plan_args.seconds = 600
        trial.plan(self.plan_args)
        path = self.plan_args.output/'plan.private.json'
        p, _ = trial.checked(path)
        self.assertEqual(p['surface'],'file-transfer')
        self.assertEqual(len(p['fixtures']),4)
        p['fixtures'][0]['path']='/Books/user.epub'
        path.write_text(json.dumps(p))
        with self.assertRaises(ValueError): trial.checked(path)

    def test_home_cannot_be_mixed_with_reader_or_unbounded_duration(self):
        self.plan_args.output=self.root/'bad-home'
        self.plan_args.surface='home';self.plan_args.seconds=181
        with self.assertRaises(ValueError): trial.plan(self.plan_args)
        self.assertFalse(self.plan_args.output.exists())
        self.plan_args.seconds=30
        self.plan_args.reader_epub=self.root/'reader.epub';self.plan_args.reader_epub.write_bytes(b'PK-reader')
        with self.assertRaises(ValueError): trial.plan(self.plan_args)
        self.assertFalse(self.plan_args.output.exists())

    def test_app_acceptance_is_explicit_standalone_and_bounded(self):
        self.plan_args.output=self.root/'app-plan';self.plan_args.seconds=600
        with self.assertRaises(ValueError): trial.plan(self.plan_args)
        self.plan_args.app_acceptance=True
        trial.plan(self.plan_args)
        path=self.plan_args.output/'plan.private.json';p,_=trial.checked(path)
        self.assertIs(p['appAcceptance'],True);self.assertEqual(p['surface'],'sync')
        for field,value in [('seconds',901),('surface','home'),('readerPages',20),('downloadPlan','other'),('appAcceptance',1)]:
            path.write_text(json.dumps({**p,field:value}))
            with self.assertRaises(ValueError):trial.checked(path)
        reader=self.root/'valid.epub';reader.write_bytes(b'PK-owned-test')
        self.image.write_bytes(self.image.read_bytes()+b'POCKET_TRIAL_READER_V1')
        for i,kwargs in enumerate((dict(seconds=901),dict(surface='home'),dict(reader_epub=reader),dict(download_plan=self.root/'unused.json'))):
            args=argparse.Namespace(**{**vars(self.plan_args),'output':self.root/('bad-app-'+str(i)),**kwargs})
            with self.assertRaises(ValueError):trial.plan(args)

    def test_home_requires_its_own_successful_memory_checkpoint(self):
        p={**self.p,'surface':'home'}
        terminal=dict(present=True,run=p['run'],phase=4,failure=0,trialSHA256=p['imageSHA256'],resetReason=3,surface='home',surfaceFree=90017,surfaceLargest=71019)
        for i, bad in enumerate((dict(surfaceFree=51199),dict(surfaceLargest=8191),dict(surface='sync'))):
            self.client.get_json.return_value={**terminal,**bad}
            out=self.root/('home-refused-'+str(i));out.mkdir()
            with patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError): trial.observe(self.client,p,out,False)
        self.client.get_json.return_value=terminal
        out=self.root/'home-pass';out.mkdir()
        with patch.object(trial,'status',return_value=self.status):trial.observe(self.client,p,out,False)
        self.assertTrue(json.loads((out/'receipt.json').read_text())['completed'])

    def test_reading_memory_gate_needs_actual_render_samples(self):
        path,p=self.reader_plan()
        good=dict(present=True,run=p['run'],phase=4,failure=0,trialSHA256=p['imageSHA256'],resetReason=8,wakeCause=4,
                  reader=dict(schema=1,run=p['run'],stage=7,requested=20,rendered=20,reopened=1,missingObserved=1,recovered=1,
                              memorySamples=20,renderMemorySamples=20,minFreeHeap=61001,minLargestBlock=47003))
        for i, bad in enumerate((dict(memorySamples=0),dict(renderMemorySamples=0),dict(minFreeHeap=51200),dict(minLargestBlock=0))):
            self.client.get_json.return_value={**good,'reader':{**good['reader'],**bad}}
            out=self.root/('memory-refused-'+str(i));out.mkdir()
            with patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError):trial.observe(self.client,p,out,False)

    def test_large_sync_download_plan_is_bound_to_same_model_identity_and_image(self):
        self.plan_args.surface='file-transfer';self.plan_args.seconds=600;self.plan_args.output=self.root/'owned-ft'
        trial.plan(self.plan_args);owner=self.plan_args.output/'plan.private.json'
        self.plan_args.surface='sync';self.plan_args.output=self.root/'sync-large';self.plan_args.download_plan=owner
        trial.plan(self.plan_args);path=self.plan_args.output/'plan.private.json';p,_=trial.checked(path)
        self.assertEqual(p['surface'],'sync');self.assertEqual(p['downloadPlanSHA256'],trial.sha(owner.read_bytes()))
        changed=json.loads(owner.read_text());changed['deviceID']='87654321';owner.write_text(json.dumps(changed))
        with self.assertRaises(ValueError):trial.checked(path)

    def test_transfer_fixture_cleanup_unknown_delete_is_single_shot(self):
        import dev_trial_transfer as ft
        self.plan_args.surface='file-transfer';self.plan_args.seconds=600;self.plan_args.output=self.root/'cleanup-ft'
        trial.plan(self.plan_args);path=self.plan_args.output/'plan.private.json';p,_=trial.checked(path)
        terminal=dict(present=True,run=p['run'],phase=4,failure=0,trialSHA256=p['imageSHA256'],surface='file-transfer')
        self.client.get_json.return_value=terminal
        data=(path.parent/p['fixtures'][0]['local']).read_bytes()
        def request(method,route,query=None,**kwargs):
            if method=='DELETE':raise TimeoutError('unknown delete reply')
            self.assertEqual(method,'GET');offset=query['offset'];return data[offset:offset+4096],{}
        self.client.request.side_effect=request
        args=self.args('cleanup');args.plan=path
        with patch.object(trial.walk,'Client',return_value=self.client),patch.object(trial,'status',return_value=self.status),self.assertRaises(TimeoutError):trial.command(args)
        self.assertEqual(sum(c.args[0]=='DELETE' for c in self.client.request.call_args_list),1)
        self.assertTrue((path.parent/'.cleanup.used').exists())
        with self.assertRaises(ValueError):trial.command(args)
        self.assertEqual(sum(c.args[0]=='DELETE' for c in self.client.request.call_args_list),1)

    def test_source_and_image_pins_are_enforced(self):
        p, _ = trial.checked(self.path)
        p['imageSHA256'] = '0'*64
        self.path.write_text(json.dumps(p))
        with self.assertRaises(ValueError):
            trial.checked(self.path)

    def test_fixture_cannot_be_changed_to_a_user_book(self):
        p, _ = trial.checked(self.path)
        p['fixture'] = '/Books/original.epub'
        self.path.write_text(json.dumps(p))
        with self.assertRaises(ValueError):
            trial.checked(self.path)

    def test_foreign_staging_path_is_refused(self):
        p, _ = trial.checked(self.path)
        p['staging'] = '/update.bin'
        self.path.write_text(json.dumps(p))
        with self.assertRaises(ValueError):
            trial.checked(self.path)

    def test_old_production_without_capability_is_refused_offline(self):
        self.plan_args.output = self.root/'old-plan'
        self.image.write_bytes((b'\xe9PocketDaily-ESP32-0.1.0\0').ljust(65536,b'\0'))
        with patch.object(trial.walk, 'Client') as client, self.assertRaises(ValueError):
            trial.plan(self.plan_args)
        client.assert_not_called()
        self.assertFalse(self.plan_args.output.exists())

    def test_execute_flag_is_required_before_any_client_is_created(self):
        with patch.object(trial.walk, 'Client') as client, self.assertRaises(ValueError):
            trial.command(self.args('mode', execute=False))
        client.assert_not_called()

    def test_unknown_install_reply_is_not_replayed(self):
        backup = self.root/'backup.json'
        backup.write_text(json.dumps({'completed':True,'prefixExact':True,'bytes':self.p['slotBytes'],
            'identitySHA256':trial.sha(self.p['deviceID'].encode()),'expectedVersion':self.p['expectedVersion'],
            'sha256':'a'*64}))
        self.client.get_json.return_value = {'present':False}
        self.client.opener.open.side_effect = TimeoutError('unknown installation outcome')
        with patch.object(trial.walk,'Client',return_value=self.client), \
             patch.object(trial,'status',return_value=self.status), self.assertRaises(TimeoutError):
            trial.command(self.args('start',return_backup=backup))
        self.assertEqual(self.client.opener.open.call_count,1)
        self.assertTrue((self.root/'start/intent.json').exists())
        # A second invocation cannot overwrite the first intent or send again.
        with self.assertRaises(ValueError):
            trial.command(self.args('start',return_backup=backup))
        self.assertEqual(self.client.opener.open.call_count,1)
        # A different output directory cannot bypass the plan-wide one-shot.
        with patch.object(trial.walk,'Client',return_value=self.client), \
             patch.object(trial,'status',return_value=self.status), self.assertRaises(FileExistsError):
            trial.command(self.args('start',return_backup=backup,output=self.root/'another-start'))
        self.assertEqual(self.client.opener.open.call_count,1)

    def test_foreign_backup_refused_before_mutation(self):
        backup=self.root/'foreign.json'
        backup.write_text(json.dumps({'completed':True,'prefixExact':True,'bytes':self.p['slotBytes'],
            'identitySHA256':'b'*64,'expectedVersion':self.p['expectedVersion'],'sha256':'a'*64}))
        with patch.object(trial.walk,'Client',return_value=self.client), \
             patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError):
            trial.command(self.args('start',return_backup=backup))
        self.client.opener.open.assert_not_called()

    def test_transfer_transport_error_is_not_swallowed_by_get_observation(self):
        out=self.root/'observation';out.mkdir()
        production={**self.status,'version':self.p['productionVersion'],
            'trialLease':{'version':4,'run':self.p['run'],'imageSHA256':self.p['imageSHA256'],
                          'seconds':self.p['seconds'],'slot':self.p['slotAddresses'][1]}}
        self.client.get_json.return_value={}
        with patch.object(trial,'status',return_value=production) as status, \
             patch.object(trial.walk,'inventory',return_value=[]), \
             patch.object(trial.put,'upload_once',side_effect=TimeoutError('ambiguous payload')) as upload, \
             patch.object(trial.put,'commit') as commit, self.assertRaises(TimeoutError):
            trial.observe(self.client,self.p,out,True)
        self.assertEqual(status.call_count,1)
        self.assertEqual(upload.call_count,1)
        commit.assert_not_called()
        self.assertTrue((out/'transfer-intent.private.json').exists())

    def test_foreign_production_witness_cannot_pass(self):
        out=self.root/'foreign-witness';out.mkdir()
        production={**self.status,'version':self.p['productionVersion'],
                    'trialLease':{'version':4,'run':self.p['run']+1}}
        with patch.object(trial,'status',return_value=production), self.assertRaises(ValueError):
            trial.observe(self.client,self.p,out,False)
        self.client.request.assert_not_called()

    def test_failed_return_is_preserved_not_cleared(self):
        out=self.root/'failed-return';out.mkdir()
        self.client.get_json.return_value={'present':True,'run':self.p['run'],'phase':5,'failure':3}
        with patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError):
            trial.observe(self.client,self.p,out,False)
        self.client.request.assert_not_called()
        self.assertEqual(json.loads((out/'returned.private.json').read_text())['phase'],5)

    def test_stage_upload_failure_never_commits(self):
        backup=self.root/'known-backup.json'
        backup.write_text(json.dumps({'completed':True,'identitySHA256':trial.sha(self.p['deviceID'].encode())}))
        with patch.object(trial.walk,'Client',return_value=self.client), \
             patch.object(trial,'status',return_value=self.status), \
             patch.object(trial.put,'upload_once',side_effect=OSError('unknown')) as upload, \
             patch.object(trial.put,'commit') as commit, self.assertRaises(OSError):
            trial.command(self.args('stage',backup_receipt=backup))
        self.assertEqual(upload.call_count,1)
        commit.assert_not_called()

    def test_foreign_terminal_record_cannot_be_cleared(self):
        self.client.get_json.return_value={'present':True,'run':self.p['run']+1,'phase':4}
        with patch.object(trial.walk,'Client',return_value=self.client), \
             patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError):
            trial.command(self.args('clear'))
        self.client.request.assert_not_called()

    def test_x3_plan_binds_observed_larger_layout(self):
        self.assertEqual(self.p['slotAddresses'], [0x10000, 0x780000])
        self.assertEqual(self.p['slotBytes'], 0x770000)

    def test_foreign_layout_size_cannot_change_plan(self):
        p = dict(self.p)
        p['slotBytes'] = 0x640000
        self.path.write_text(json.dumps(p))
        with self.assertRaises(ValueError):
            trial.checked(self.path)

    def test_image_export_refuses_x4_layout_on_x3_before_any_piece(self):
        self.client.get_json.return_value = {'schema':4, 'capability':trial.CAPABILITY[:-1].decode(),
            'runningAddress':0x10000, 'nextAddress':0x650000,
            'runningBytes':0x640000, 'nextBytes':0x640000}
        out = self.root/'wrong-layout'
        out.mkdir()
        with patch.object(trial,'status',return_value=self.status), self.assertRaises(ValueError):
            trial.backup_image(self.client,self.p,out,self.image)
        self.client.request.assert_not_called()
        self.assertFalse((out/'return-slot.private.bin').exists())


if __name__ == '__main__':
    unittest.main()
